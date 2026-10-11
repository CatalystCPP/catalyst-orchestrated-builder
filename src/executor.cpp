// Copyright 2026 Siddharth Mohanty
// SPDX-License-Identifier: Apache-2.0

#include "cob/executor.hpp"

#include "cob/binary.hpp"
#include "cob/build_step.hpp"
#include "cob/builder.hpp"
#include "cob/graph.hpp"
#if FF_cob__json_impl
#include "cob/json.hpp"
#endif
#include "cob/mpmc_queue.hpp"
#include "cob/mpsc_queue.hpp"
#include "cob/process_exec.hpp"
#include "cob/utility.hpp"
#include "cob/utils.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <ostream>
#include <print>
#include <ranges>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>

#ifdef __linux__
#include <cerrno>
#include <fcntl.h> // AT_FDCWD
#include <liburing.h>
#endif

inline constexpr size_t PROGRESS_QUEUE_SZ = FF_cob__progress_queue_size;
inline constexpr size_t BUF_INIT_CAPACITY = FF_cob__buffer_initial_capacity; // default: 2MB
inline constexpr size_t FLUSH_THRESHOLD = FF_cob__flush_threshold;           // default: 1MB
inline constexpr size_t RSP_INPUT_THRESHOLD = FF_cob__rsp_input_threshold;
inline constexpr size_t RSP_LEN_ESTIMATE =
    FF_cob__rsp_path_estimate; // estimate on the size of a single RSP element path.
#if FF_cob__heterogeneous_core_affinity
static constexpr size_t HEAVY_TASK_THRESHOLD = FF_cob__heavy_task_threshold;
#endif

namespace fs = std::filesystem;

#if FF_cob__heterogeneous_core_affinity
#include <sys/resource.h>
#endif

namespace catalyst {

[[clang::always_inline]]
inline std::string escapeJSONString(std::string_view s) {
    std::string res;
    res.reserve(s.size() + 2);
    res.push_back('"');
    for (char c : s) {
        switch (c) {
            case '"':
                res.append("\\\"");
                break;
            case '\\':
                res.append("\\\\");
                break;
            case '\b':
                res.append("\\b");
                break;
            case '\f':
                res.append("\\f");
                break;
            case '\n':
                res.append("\\n");
                break;
            case '\r':
                res.append("\\r");
                break;
            case '\t':
                res.append("\\t");
                break;
            default: {
                constexpr unsigned char ESCAPE_THRESHOLD = 0x20;
                constexpr unsigned char MAX_SAFE_CHAR = 7;
                const auto uc = static_cast<unsigned char>(c);
                if (uc < ESCAPE_THRESHOLD) {
                    std::array<char, MAX_SAFE_CHAR> buf{};
                    auto [out, size] =
                        std::format_to_n(buf.data(), buf.size(), "\\u{:04x}", static_cast<unsigned int>(uc));
                    res.append(buf.data(), out - buf.data());
                } else {
                    res.push_back(c);
                }
                break;
            }
        }
    }
    res.push_back('"');
    return res;
}

#if FF_cob__json_impl
void dumpJSONInternal(const catalyst::JSON &j, std::string &buf) {
    switch (j.type) {
        case catalyst::JSON::Type::Null:
            buf.append("null");
            break;
        case catalyst::JSON::Type::Boolean:
            buf.append(j.bool_val ? "true" : "false");
            break;
        case catalyst::JSON::Type::Number:
            buf.append(std::to_string(j.num_val));
            break;
        case catalyst::JSON::Type::String:
            buf.append(escapeJSONString(j.str_val));
            break;
        case catalyst::JSON::Type::Array: {
            const size_t array_sz = j.arr_val.size();
            if (array_sz == 0) {
                buf.append("[]");
                break;
            }
            buf.append("[");
            for (size_t i = 0; i < array_sz; ++i) {
                dumpJSONInternal(j.arr_val[i], buf);
                if (i + 1 < array_sz) [[likely]]
                    buf.append(",");
            }
            buf.append("]");
            break;
        }
        case catalyst::JSON::Type::Object: {
            if (j.obj_val.empty()) {
                buf.append("{}");
                break;
            }
            buf.append("{");
            size_t i = 0;
            for (auto it = j.obj_val.begin(); it != j.obj_val.end(); ++it, ++i) {
                buf.append(escapeJSONString(it->first));
                buf.append(":");
                dumpJSONInternal(it->second, buf);
                if (i + 1 < j.obj_val.size()) {
                    buf.append(",");
                }
            }
            buf.append("}");
            break;
        }
    }
}

void dumpJSON(const catalyst::JSON &j, std::ofstream &os) {
    std::string buf;
    buf.reserve(1 << 20);
    dumpJSONInternal(j, buf);
    os.write(buf.data(), static_cast<long>(buf.size()));
}
#endif

bool isNewer(const std::filesystem::path &new_file, const std::filesystem::path &old_file) {
    std::error_code ec;
    fs::file_time_type new_time = std::filesystem::last_write_time(new_file, ec);
    if (ec)
        return true;
    fs::file_time_type old_time = std::filesystem::last_write_time(old_file, ec);
    if (ec)
        return true;
    return new_time > old_time;
}

Executor::Executor(COBBuilder &&builder, ExecutorConfig config)
    : builder(std::move(builder)), config(std::move(config)) {
#if FF_cob__estimates
    estimator = std::make_unique<WorkEstimate>(config.estimates_file);
#endif
}

#ifdef __linux__
namespace {
constexpr unsigned CLEAN_QUEUE_DEPTH = FF_cob__clean_ring_capacity;

void unlinkSynchronously(std::span<const std::string> paths) {
    for (const std::string &path : paths) {
        ::unlink(path.c_str());
    }
}

void unlinkWithIoUring(std::span<const std::string> paths) {
    io_uring ring{};
    if (io_uring_queue_init(CLEAN_QUEUE_DEPTH, &ring, 0) < 0) {
        unlinkSynchronously(paths);
        return;
    }

    size_t offset = 0;
    while (offset < paths.size()) {
        const size_t remaining = paths.size() - offset;
        const auto count = static_cast<unsigned>(remaining < CLEAN_QUEUE_DEPTH ? remaining : CLEAN_QUEUE_DEPTH);

        for (unsigned i = 0; i < count; ++i) {
            io_uring_sqe *sqe = io_uring_get_sqe(&ring);
            io_uring_prep_unlinkat(sqe, AT_FDCWD, paths[offset + i].c_str(), 0);
            io_uring_sqe_set_data64(sqe, offset + i);
        }

        unsigned submitted = 0;
        while (submitted < count) {
            const int result = io_uring_submit(&ring);
            if (result == -EINTR) {
                continue;
            }
            if (result <= 0) {
                break;
            }
            submitted += static_cast<unsigned>(result);
        }

        io_uring_cqe *first_completion = nullptr;
        int wait_result = 0;
        if (submitted > 0) {
            wait_result = io_uring_wait_cqe_nr(&ring, &first_completion, submitted);
            while (wait_result == -EINTR) {
                wait_result = io_uring_wait_cqe_nr(&ring, &first_completion, submitted);
            }
        }

        if (submitted != count || wait_result < 0) {
            io_uring_queue_exit(&ring);
            unlinkSynchronously(paths.subspan(offset));
            return;
        }

        for (unsigned i = 0; i < submitted; ++i) {
            io_uring_cqe *completion = first_completion;
            if (i > 0) {
                io_uring_peek_cqe(&ring, &completion);
            }
            if (completion->res < 0 && completion->res != -ENOENT) {
                ::unlink(paths[io_uring_cqe_get_data64(completion)].c_str());
            }
            io_uring_cqe_seen(&ring, completion);
        }
        offset += count;
    }

    io_uring_queue_exit(&ring);
}
} // namespace
#endif

Result<void> Executor::clean() {
    catalyst::BuildGraph build_graph = builder.emitGraph();
    std::println("Cleaning build artifacts...");

#ifdef __linux__
    std::vector<std::string> paths;
    paths.reserve(build_graph.steps().size() * 2);
#else
    std::error_code ec; // used to make std::filesystem::remove noexcept
#endif

    for (const BuildStep &step : build_graph.steps()) {
        const bool is_compile_step = step.tool[0] == 'c';

        if (config.clean_cc_only && !is_compile_step) [[unlikely]]
            continue;

#ifdef __linux__
        paths.emplace_back(step.output);
        if (is_compile_step)
            paths.emplace_back(std::string(step.output) + ".d");
#else
        std::filesystem::remove(std::filesystem::path(step.output), ec);
        if (is_compile_step)
            std::filesystem::remove(std::string(step.output) + ".d", ec);
#endif
    }

#ifdef __linux__
    unlinkWithIoUring(paths);
#endif
    return {};
}

namespace {
[[clang::always_inline]]
inline uint64_t hashCommand(std::span<const std::string> args) {
    uint64_t hash = 0;
    for (const std::string &arg : args) {
        hash = rapid_hash(arg, hash);
        hash = rapid_hash(" ", hash);
    }
    return hash;
}
} // namespace

/**
 * @brief Modification time cache indexed by graph node.
 *
 * Every parsed, opaque, and depfile input of a step is a graph node with an edge to the step's output, so a
 * step's inputs are exactly its node's in-edges. Indexing by node replaces per-lookup hashing, locking, and
 * string comparison with an array access. Slots are filled at most once without locks; a thread that races
 * a fill uses its own stat result. Produced nodes are only read after their producer finished (an edge
 * orders them), so the producer may reset or record its output slot without synchronisation.
 */
class Executor::NodeTimes {
public:
    NodeTimes(const BuildGraph &graph, std::string_view build_file)
        : nodes(graph.nodes()), slots(graph.nodes().size()), build_file(build_file),
          build_file_node(graph.findNode(build_file)) {
        const size_t node_count = nodes.size();
        in_offsets.assign(node_count + 1, 0);
        for (const BuildGraph::Node &node : nodes) {
            for (size_t out : node.out_edges) {
                ++in_offsets[out + 1];
            }
        }
        for (size_t i = 0; i < node_count; ++i) {
            in_offsets[i + 1] += in_offsets[i];
        }
        in_nodes.resize(in_offsets[node_count]);
        std::vector<size_t> cursor(in_offsets.begin(), in_offsets.end() - 1);
        for (size_t i = 0; i < node_count; ++i) {
            for (size_t out : nodes[i].out_edges) {
                in_nodes[cursor[out]++] = i;
            }
        }
    }

    /** @brief The nodes this node's step consumes (parsed, opaque, and depfile inputs). */
    [[nodiscard]] std::span<const size_t> inputsOf(size_t node) const {
        return std::span{in_nodes}.subspan(in_offsets[node], in_offsets[node + 1] - in_offsets[node]);
    }

    /** @brief True if the input is missing or not older than output_time. */
    bool changedSince(size_t node, fs::file_time_type output_time) {
        return changedSince(slots[node], nodes[node].path, output_time);
    }

    bool buildFileChangedSince(fs::file_time_type output_time) {
        return changedSince(build_file_slot, build_file, output_time);
    }

    /** @brief Records a time already observed for a node (e.g. an up-to-date output). */
    void record(size_t node, fs::file_time_type time) {
        Slot &slot = slots[node];
        uint8_t expected = UNKNOWN;
        if (slot.state.compare_exchange_strong(expected, BUSY, std::memory_order_acquire)) {
            slot.time = time;
            slot.missing = false;
            slot.state.store(READY, std::memory_order_release);
        }
    }

    /** @brief Drops a node's cached time after its file was rewritten. */
    void invalidate(size_t node) {
        slots[node].state.store(UNKNOWN, std::memory_order_release);
        if (build_file_node == node) {
            build_file_slot.state.store(UNKNOWN, std::memory_order_release);
        }
    }

private:
    static constexpr uint8_t UNKNOWN = 0;
    static constexpr uint8_t BUSY = 1;
    static constexpr uint8_t READY = 2;

    struct Slot {
        std::atomic<uint8_t> state{UNKNOWN};
        bool missing = false;
        fs::file_time_type time;
    };

    static bool changedSince(Slot &slot, std::string_view path, fs::file_time_type output_time) {
        if (slot.state.load(std::memory_order_acquire) == READY) {
            return slot.missing || slot.time >= output_time;
        }

        std::error_code ec;
        const fs::file_time_type time = fs::last_write_time(fs::path(path), ec);
        uint8_t expected = UNKNOWN;
        if (slot.state.compare_exchange_strong(expected, BUSY, std::memory_order_acquire)) {
            slot.time = time;
            slot.missing = static_cast<bool>(ec);
            slot.state.store(READY, std::memory_order_release);
        }
        return ec || time >= output_time;
    }

    const std::vector<BuildGraph::Node> &nodes;
    std::vector<Slot> slots;
    std::vector<size_t> in_offsets;
    std::vector<size_t> in_nodes;
    std::string_view build_file;
    std::optional<size_t> build_file_node;
    Slot build_file_slot;
};

[[clang::always_inline]]
bool inline Executor::needsRebuild(const BuildStep &step,
                                   size_t node_idx,
                                   NodeTimes &node_times,
                                   const ToolchainFlags &flags,
                                   uint64_t *out_hash,
                                   std::vector<std::string> *out_args) const {
    std::vector<std::string> args = buildCommandArgs(step, true, flags);
    uint64_t current_hash = hashCommand(std::span{args.data(), args.size()});

    if (out_hash) {
        *out_hash = current_hash;
    }

    auto rebuild = [&args, out_args]() {
        if (out_args) {
            *out_args = std::move(args);
        }
        return true;
    };

    // One stat answers both "does the output exist" and "when was it written".
    std::error_code ec;
    fs::file_time_type output_modtime = std::filesystem::last_write_time(step.output, ec);
    if (ec) {
        // Unusual failures keep the exists()-then-last_write_time() behaviour, including its exceptions.
        if (!std::filesystem::exists(step.output) || step.command_hash != current_hash) {
            return rebuild();
        }
        output_modtime = std::filesystem::last_write_time(step.output);
    } else if (step.command_hash != current_hash) {
        return rebuild();
    }

    if (node_times.buildFileChangedSince(output_modtime)) {
        return rebuild();
    }

    // Parsed, opaque, and depfile inputs (this is also how a stale .d file is detected).
    for (size_t input : node_times.inputsOf(node_idx)) {
        if (node_times.changedSince(input, output_modtime)) {
            return rebuild();
        }
    }

    // Up to date: consumers of this output can reuse the time instead of stat'ing it again.
    node_times.record(node_idx, output_modtime);
    return false;
}

Result<void> Executor::emitGraph() {
    catalyst::BuildGraph build_graph = builder.emitGraph();
    NodeTimes node_times(build_graph, config.build_file);

    const auto cc_vec = builder.getDefinitionOf<std::vector<std::string>>("cc");
    const auto cxx_vec = builder.getDefinitionOf<std::vector<std::string>>("cxx");
    const auto linker_vec = builder.getLinkerVec(cxx_vec);
    const auto archiver_vec = builder.getArchiverVec();
    const auto cflags_vec = builder.getDefinitionOf<std::vector<std::string>>("cflags");
    const auto cxxflags_vec = builder.getDefinitionOf<std::vector<std::string>>("cxxflags");
    const auto ldflags_vec = builder.getDefinitionOf<std::vector<std::string>>("ldflags");
    const auto ldlibs_vec = builder.getDefinitionOf<std::vector<std::string>>("ldlibs");

    ToolchainFlags tf = {.cc = cc_vec,
                         .cxx = cxx_vec,
                         .linker = linker_vec,
                         .archiver = archiver_vec,
                         .cflags = cflags_vec,
                         .cxxflags = cxxflags_vec,
                         .ldflags = ldflags_vec,
                         .ldlibs = ldlibs_vec};

    std::cout << "digraph catalyst_build {\n";
    std::cout << "  rankdir=LR;\n";
    std::cout << "  node [shape=box, style=filled, fontname=\"Helvetica\"];\n";

    for (size_t i = 0; i < build_graph.nodes().size(); ++i) {
        const BuildGraph::Node &node = build_graph.nodes()[i];
        std::string color = "0.9 0.9 0.9"; // light gray for source files

        if (node.step_id.has_value()) {
            const BuildStep &step = build_graph.steps()[*node.step_id];
            if (needsRebuild(step, i, node_times, tf)) {
                color = "green";
            } else {
                color = "white";
            }
        }

        std::cout << "  n" << i << " [label=\"" << node.path << "\", fillcolor=\"" << color << "\"];\n";

        for (size_t target_idx : node.out_edges) {
            std::cout << "  n" << i << " -> n" << target_idx << ";\n";
        }
    }
    std::cout << "}\n";
    return {};
}

Result<void> Executor::emitCompDB() {
    catalyst::BuildGraph build_graph = builder.emitGraph();
    std::ofstream f("compile_commands.json");
    std::string cwd = std::filesystem::current_path().string();

    const auto cc_vec = builder.getDefinitionOf<std::vector<std::string>>("cc");
    const auto cxx_vec = builder.getDefinitionOf<std::vector<std::string>>("cxx");
    const auto linker_vec = builder.getLinkerVec(cxx_vec);
    const auto archiver_vec = builder.getArchiverVec();
    const auto cflags_vec = builder.getDefinitionOf<std::vector<std::string>>("cflags");
    const auto cxxflags_vec = builder.getDefinitionOf<std::vector<std::string>>("cxxflags");
    const auto ldflags_vec = builder.getDefinitionOf<std::vector<std::string>>("ldflags");
    const auto ldlibs_vec = builder.getDefinitionOf<std::vector<std::string>>("ldlibs");

    ToolchainFlags tf = {.cc = cc_vec,
                         .cxx = cxx_vec,
                         .linker = linker_vec,
                         .archiver = archiver_vec,
                         .cflags = cflags_vec,
                         .cxxflags = cxxflags_vec,
                         .ldflags = ldflags_vec,
                         .ldlibs = ldlibs_vec};

    std::string buf;
    buf.reserve(BUF_INIT_CAPACITY);
    buf.append("[\n");
    for (bool first = true; const BuildGraph::Node &node : build_graph.nodes()) {
        if (!node.step_id.has_value())
            continue;
        const BuildStep &step = build_graph.steps()[*node.step_id];

        if (step.tool != "cc" && step.tool != "cxx")
            continue;

        if (!first) {
            buf.append(",\n");
        }
        first = false;

        const std::vector<std::string_view> &inputs = step.parsed_inputs;
        std::vector<std::string> args = buildCommandArgs(step, true, tf);

        buf.append("  {\n    \"directory\": ");
        buf.append(escapeJSONString(cwd));
        buf.append(",\n    \"arguments\": [");
        for (size_t i = 0; i < args.size(); ++i) {
            buf.append(escapeJSONString(args[i]));
            if (i + 1 < args.size())
                buf.append(", ");
        }
        buf.append("],\n");

        if (!inputs.empty()) {
            buf.append("    \"file\": ");
            buf.append(escapeJSONString(inputs[0]));
            buf.append(",\n");
        }
        buf.append("    \"output\": ");
        buf.append(escapeJSONString(step.output));
        buf.append("\n  }");

        if (buf.size() > FLUSH_THRESHOLD) {
            f.write(buf.data(), static_cast<long>(buf.size()));
            buf.clear();
        }
    }
    buf.append("\n]\n");
    f.write(buf.data(), static_cast<long>(buf.size()));

    return {};
}

Result<void> Executor::emitCommands() {
    catalyst::BuildGraph build_graph = builder.emitGraph();
    std::vector<size_t> order;
    auto res = build_graph.topoSort();
    if (!res)
        return std::unexpected(res.error());
    order = *res;

    const auto cc_vec = builder.getDefinitionOf<std::vector<std::string>>("cc");
    const auto cxx_vec = builder.getDefinitionOf<std::vector<std::string>>("cxx");
    const auto linker_vec = builder.getLinkerVec(cxx_vec);
    const auto archiver_vec = builder.getArchiverVec();
    const auto cflags_vec = builder.getDefinitionOf<std::vector<std::string>>("cflags");
    const auto cxxflags_vec = builder.getDefinitionOf<std::vector<std::string>>("cxxflags");
    const auto ldflags_vec = builder.getDefinitionOf<std::vector<std::string>>("ldflags");
    const auto ldlibs_vec = builder.getDefinitionOf<std::vector<std::string>>("ldlibs");

    ToolchainFlags tf = {.cc = cc_vec,
                         .cxx = cxx_vec,
                         .linker = linker_vec,
                         .archiver = archiver_vec,
                         .cflags = cflags_vec,
                         .cxxflags = cxxflags_vec,
                         .ldflags = ldflags_vec,
                         .ldlibs = ldlibs_vec};

    for (size_t node_idx : order) {
        const BuildGraph::Node &node = build_graph.nodes()[node_idx];
        if (!node.step_id.has_value())
            continue;
        const BuildStep &step = build_graph.steps()[*node.step_id];
        std::vector<std::string> args = buildCommandArgs(step, true, tf);
        for (size_t i = 0; i < args.size(); ++i) {
            std::cout << args[i] << (i == args.size() - 1 ? "" : " ");
        }
        std::cout << "\n";
    }
    return {};
}

struct Executor::ExecuteContext {
    struct Task {
        size_t node_idx;
        size_t estimate;
        bool operator<(const Task &other) const {
            return estimate < other.estimate;
        }
    };

    struct LogEvent {
        std::string console_message;
        std::string log_message;
        bool is_error = false;
        bool is_poison_pill = false;
    };

    LockFreeMPMCQueue<Task> ready_queue;
    LockFreeMPSCQueue<LogEvent> progress_queue;
    std::atomic<size_t> tasks_available{0};
    std::atomic<size_t> sleeping_threads{0};
    std::atomic<size_t> messages_enqueued{0};
    std::atomic<size_t> completed_count{0};
    std::atomic<size_t> steps_completed{0};
    size_t total_nodes{0};
    size_t steps_to_build{0};
    std::vector<std::atomic<int>> in_degrees;
    const std::vector<std::string> cc_vec;
    const std::vector<std::string> cxx_vec;
    const std::vector<std::string> linker_vec;
    const std::vector<std::string> archiver_vec;
    const std::vector<std::string> cflags_vec;
    const std::vector<std::string> cxxflags_vec;
    const std::vector<std::string> ldflags_vec;
    const std::vector<std::string> ldlibs_vec;
    catalyst::BuildGraph build_graph;
    std::atomic<bool> error_occurred{false};
    std::atomic<bool> hashes_changed{false}; ///< Whether the graph differs from the loaded cache.

    ExecuteContext(size_t node_count,
                   std::vector<std::string> cc,
                   std::vector<std::string> cxx,
                   std::vector<std::string> linker,
                   std::vector<std::string> archiver,
                   std::vector<std::string> cflags,
                   std::vector<std::string> cxxflags,
                   std::vector<std::string> ldflags,
                   std::vector<std::string> ldlibs,
                   catalyst::BuildGraph bg)
        : ready_queue(node_count), progress_queue(PROGRESS_QUEUE_SZ), total_nodes(node_count), in_degrees(node_count),
          cc_vec(std::move(cc)), cxx_vec(std::move(cxx)), linker_vec(std::move(linker)),
          archiver_vec(std::move(archiver)), cflags_vec(std::move(cflags)), cxxflags_vec(std::move(cxxflags)),
          ldflags_vec(std::move(ldflags)), ldlibs_vec(std::move(ldlibs)), build_graph(std::move(bg)) {
    }
};

/**
 * @brief Evaluates whether a build step needs to be re-run based on file timestamps.
 */
// needs_rebuild is already defined earlier

/**
 * @brief Generates the command-line arguments for a specific build step.
 *
 * Includes logic for resolving the appropriate toolchain commands and generating
 * response (.rsp) files for linking steps with many inputs to avoid command-line
 * length limits.
 */
std::vector<std::string>
Executor::buildCommandArgs(const BuildStep &step, bool dry_run_mode, const ToolchainFlags &flags) const {
    std::vector<std::string> args;
    static constexpr size_t ARGS_VEC_INIT_SZ = 40Z;
    args.reserve(ARGS_VEC_INIT_SZ);

    auto add_parts = [&args](const auto &parts) {
        args.reserve(args.size() + parts.size());
        for (const auto &part : parts) {
            if (part.begin() != part.end()) {
                args.push_back(std::ranges::to<std::string>(part));
            }
        }
    };

    const std::vector<std::string_view> &inputs = step.parsed_inputs;

    auto add_extra_flags = [&args](std::string_view extra) {
        size_t start = 0;
        while (start < extra.size()) {
            size_t word_start = extra.find_first_not_of(" \t", start);
            if (word_start == std::string_view::npos) {
                break;
            }
            size_t word_end = extra.find_first_of(" \t", word_start);
            if (word_end == std::string_view::npos) {
                args.emplace_back(extra.substr(word_start));
                break;
            }
            args.emplace_back(extra.substr(word_start, word_end - word_start));
            start = word_end;
        }
    };

    if (step.tool == "cc") {
        add_parts(flags.cc);
        add_parts(flags.cflags);
        add_extra_flags(step.extra_flags);
        args.insert(args.end(),
                    {"-MMD", "-MT", std::string(step.output), "-MF", std::string(step.output) + ".d", "-c"});
        for (const std::string_view &in : inputs)
            args.emplace_back(in);
        args.emplace_back("-o");
        args.emplace_back(step.output);
    } else if (step.tool == "cxx") {
        add_parts(flags.cxx);
        add_parts(flags.cxxflags);
        add_extra_flags(step.extra_flags);
        args.insert(args.end(),
                    {"-MMD", "-MT", std::string(step.output), "-MF", std::string(step.output) + ".d", "-c"});
        for (const std::string_view &in : inputs)
            args.emplace_back(in);
        args.emplace_back("-o");
        args.emplace_back(step.output);
    } else if (step.tool == "ld") {
        add_parts(flags.linker);
        std::filesystem::path rsp_path = std::filesystem::path(step.output).replace_extension(".rsp");

        bool use_rsp = false;
        if (std::filesystem::exists(rsp_path) && isNewer(rsp_path, config.build_file)) {
            use_rsp = true;
        } else if (inputs.size() > RSP_INPUT_THRESHOLD) {
            use_rsp = true;
            if (!dry_run_mode) {
                std::string rsp_content;
                rsp_content.reserve(inputs.size() * RSP_LEN_ESTIMATE);
                for (const std::string_view &input : inputs) {
                    rsp_content += input;
                    rsp_content += '\n';
                }
                std::ofstream rsp_file(rsp_path);
                rsp_file.write(rsp_content.data(), static_cast<long>(rsp_content.size()));
            }
        }

        if (use_rsp) {
            args.push_back(std::string("@") + rsp_path.string());
        } else {
            for (const std::string_view &in : inputs)
                args.emplace_back(in);
        }
        args.emplace_back("-o");
        args.emplace_back(step.output);
        add_parts(flags.ldflags);
        add_parts(flags.ldlibs);
        add_extra_flags(step.extra_flags);
    } else if (step.tool == "ar") {
        add_parts(flags.archiver);
        if (!flags.archiver.empty()) {
            const std::string &cmd = flags.archiver.front();
            if (cmd.ends_with("ar") || cmd.ends_with("ar.exe") || flags.archiver.size() == 1) {
                args.emplace_back("rcs");
                args.emplace_back(step.output);
            } else if (flags.archiver.back() == "/out:") {
                args.back() += std::string(step.output);
            } else {
                args.emplace_back(step.output);
            }
        } else {
            args.insert(args.end(), {"ar", "rcs", std::string(step.output)});
        }
        for (const std::string_view &in : inputs)
            args.emplace_back(in);
        add_extra_flags(step.extra_flags);
    } else if (step.tool == "sld") {
        add_parts(flags.linker);
        args.emplace_back("-shared");
        for (const std::string_view &in : inputs)
            args.emplace_back(in);
        args.emplace_back("-o");
        args.emplace_back(step.output);
        add_parts(flags.ldflags);
        add_parts(flags.ldlibs);
        add_extra_flags(step.extra_flags);
    }
    return args;
}

/**
 * @brief Enqueues a task representing a node ready to be processed.
 *
 * If work estimates are enabled, calculates the estimate for the node and assigns it.
 * Utilizes the lock-free queue and notifies a sleeping worker thread via atomic wait primitives.
 */
#if FF_cob__estimates
void Executor::pushReady(size_t idx, ExecuteContext &ctx, WorkEstimate *estimator) {
#else
void Executor::pushReady(size_t idx, ExecuteContext &ctx, [[maybe_unused]] void *estimator) {
#endif
    size_t est = 0;
#if FF_cob__estimates
    if (estimator) {
        const auto &node = ctx.build_graph.nodes()[idx];
        if (node.step_id.has_value()) {
            est = estimator->getWorkEstimate(ctx.build_graph.steps()[*node.step_id].output);
        }
    }
#endif
#ifdef DEBUG
    bool enqueued = ctx.ready_queue.enqueue({.node_idx = idx, .estimate = est});
    assert(enqueued && "Invariant violated: ready_queue overflow");
#else
    ctx.ready_queue.enqueue({.node_idx = idx, .estimate = est});
#endif
    ctx.tasks_available.fetch_add(1, std::memory_order_release);
    ctx.tasks_available.notify_one();
}

/**
 * @brief Formats and prints a thread-safe progress message for a build step.
 *
 * Uses terminal color codes if stdout is a TTY and updates the line dynamically.
 */
void Executor::printMessage(const BuildStep &step, ExecuteContext &ctx, bool is_tty) const {
    if (config.silent) {
        return;
    }

    std::string raw_action = std::string(step.tool);
    std::string target = std::string(step.output);

    if (step.tool == "cc" || step.tool == "cxx") {
        raw_action = "Compiling";
        if (!step.parsed_inputs.empty()) {
            target = std::string(step.parsed_inputs[0]);
        }
    } else if (step.tool == "ld") {
        raw_action = "Linking";
    } else if (step.tool == "sld") {
        raw_action = "Linking library";
    } else if (step.tool == "ar") {
        raw_action = "Archiving";
    }

    std::string action;
    if (is_tty) {
        std::string color_code;
        if (step.tool == "cc" || step.tool == "cxx") {
            color_code = "\033[36m"; // Cyan
        } else if (step.tool == "ld") {
            color_code = "\033[32m"; // Green
        } else if (step.tool == "sld") {
            color_code = "\033[35m"; // Magenta
        } else if (step.tool == "ar") {
            color_code = "\033[33m"; // Yellow
        }

        std::string padded = std::format("{:<15}", raw_action);
        if (!color_code.empty()) {
            action = std::format("{}{}\033[0m", color_code, padded);
        } else {
            action = padded;
        }
    } else {
        action = raw_action;
    }

    std::string msg;
    if (config.dry_run) {
        msg = std::format("[DRY RUN] {} {}\n", action, target);
    } else {
        size_t current = ctx.steps_completed.fetch_add(1, std::memory_order_relaxed) + 1;
        if (is_tty) {
            msg = std::format("\r\033[K[{}] {} {}", current, action, target);
        } else {
            msg = std::format("[{}] {:<15} {}\n", current, action, target);
        }
    }

    while (!ctx.progress_queue.enqueue(
        {.console_message = msg, .log_message = "", .is_error = false, .is_poison_pill = false}))
        std::this_thread::yield();
    ctx.messages_enqueued.fetch_add(1, std::memory_order_release);
    ctx.messages_enqueued.notify_one();
}

/**
 * @brief Processes a single build graph node.
 *
 * Verifies if the node requires rebuilding, generates its command, invokes the process
 * executor, and captures/logs output in a thread-safe manner. Returns 0 on success.
 */
int Executor::processStep(size_t node_idx, ExecuteContext &ctx, NodeTimes &node_times, bool is_tty) const {
    // NOLINTBEGIN(performance-avoid-endl)
    const BuildGraph::Node &node = ctx.build_graph.nodes()[node_idx];
    if (node.step_id.has_value()) {
        BuildStep &step = ctx.build_graph.steps()[*node.step_id];

#if FF_cob__logging
        const bool start_progress = !config.silent || !config.build_log_file.empty();
#else
        const bool start_progress = !config.silent;
#endif

        ToolchainFlags tf = {.cc = ctx.cc_vec,
                             .cxx = ctx.cxx_vec,
                             .linker = ctx.linker_vec,
                             .archiver = ctx.archiver_vec,
                             .cflags = ctx.cflags_vec,
                             .cxxflags = ctx.cxxflags_vec,
                             .ldflags = ctx.ldflags_vec,
                             .ldlibs = ctx.ldlibs_vec};

        uint64_t step_hash = 0;
        std::vector<std::string> args;
        if (needsRebuild(step, node_idx, node_times, tf, &step_hash, &args)) {
            printMessage(step, ctx, is_tty);
            if (config.dry_run)
                return 0;

            if (step.tool == "ld") {
                args = buildCommandArgs(step, false, tf);
            }

#if FF_cob__profiling
            auto start = std::chrono::steady_clock::now();
#endif
#if FF_cob__logging
            bool capture_output = !config.build_log_file.empty();
#else
            bool capture_output = false;
#endif
            auto res = catalyst::process_exec(args, std::nullopt, std::nullopt, capture_output);
#if FF_cob__profiling
            auto end = std::chrono::steady_clock::now();
            std::chrono::duration<double> diff = end - start;
            {
                std::string msg = std::format("Step {} took {:.4f}s\n", step.output, diff.count());
                if (start_progress) {
                    while (!ctx.progress_queue.enqueue({msg, "", false, false}))
                        std::this_thread::yield();
                    ctx.messages_enqueued.fetch_add(1, std::memory_order_release);
                    ctx.messages_enqueued.notify_one();
                } else {
                    std::print("{}", msg);
                }
            }
#endif

            if (res) {
                auto [ec, output] = *res;
#if FF_cob__logging
                if (capture_output && !output.empty()) {
                    std::string console_msg;
                    if (!config.silent || ec != 0) {
                        console_msg = output;
                    }

                    std::string log_msg;
                    if (!config.build_log_file.empty()) {
                        log_msg = std::format("=== {} -> {} ===\n{}", step.tool, step.output, output);
                        if (output.back() != '\n')
                            log_msg += '\n';
                        log_msg += '\n';
                    }

                    if (!console_msg.empty() || !log_msg.empty()) {
                        while (!ctx.progress_queue.enqueue({console_msg, log_msg, ec != 0, false}))
                            std::this_thread::yield();
                        ctx.messages_enqueued.fetch_add(1, std::memory_order_release);
                        ctx.messages_enqueued.notify_one();
                    }
                }
#endif
                if (ec != 0) {
                    std::string err_msg =
                        std::format("Build failed: {} -> {} (exit code {})\n", step.tool, step.output, ec);
                    if (start_progress) {
                        while (!ctx.progress_queue.enqueue(
                            {.console_message = err_msg, .log_message = "", .is_error = true, .is_poison_pill = false}))
                            std::this_thread::yield();
                        ctx.messages_enqueued.fetch_add(1, std::memory_order_release);
                        ctx.messages_enqueued.notify_one();
                    } else {
                        std::print(stderr, "{}", err_msg);
                    }
                    return ec;
                }
                // build succeeded, update the command hash for future up-to-date checks
                if (step.command_hash != step_hash) {
                    step.command_hash = step_hash;
                    ctx.hashes_changed.store(true, std::memory_order_relaxed);
                }
                // the output was just rewritten; drop any stale cached modtime so downstream
                // steps consuming this artifact re-stat it and see the fresh time
                node_times.invalidate(node_idx);
            } else {
                std::string err_msg = std::format("Failed to execute: {}\n", res.error());
                if (start_progress) {
                    while (!ctx.progress_queue.enqueue(
                        {.console_message = err_msg, .log_message = "", .is_error = true, .is_poison_pill = false}))
                        std::this_thread::yield();
                    ctx.messages_enqueued.fetch_add(1, std::memory_order_release);
                    ctx.messages_enqueued.notify_one();
                } else {
                    std::print(stderr, "{}", err_msg);
                }
                return 1;
            }
        }
#if FF_cob__logging
        else {
            std::string msg = std::format("Skipping {} (up to date)\n", step.output);
            if (start_progress) {
                while (!ctx.progress_queue.enqueue({msg, "", false, false}))
                    std::this_thread::yield();
                ctx.messages_enqueued.fetch_add(1, std::memory_order_release);
                ctx.messages_enqueued.notify_one();
            }
        }
#endif
    }
    return 0;
    // NOLINTEND(performance-avoid-endl)
}

/**
 * @brief Main worker thread logic.
 *
 * Loops indefinitely retrieving tasks from the lock-free queue. Uses atomic futex waits
 * to sleep when the queue is empty. Implements deadlock detection to resolve stalled graphs.
 */
void Executor::workerLoop(ExecuteContext &ctx, NodeTimes &node_times, bool is_tty, size_t thread_count) {
    while (true) {
        ExecuteContext::Task task{};
        while (true) {
            if (ctx.ready_queue.dequeue(task)) {
                ctx.tasks_available.fetch_sub(1, std::memory_order_relaxed);
                break;
            }

            if (ctx.completed_count.load(std::memory_order_acquire) == ctx.total_nodes ||
                ctx.error_occurred.load(std::memory_order_acquire)) {
                return;
            }

            size_t curr = ctx.tasks_available.load(std::memory_order_acquire);
            if (curr == 0) {
                if (ctx.completed_count.load(std::memory_order_acquire) == ctx.total_nodes ||
                    ctx.error_occurred.load(std::memory_order_acquire)) {
                    return;
                }

                size_t sleeping = ctx.sleeping_threads.fetch_add(1, std::memory_order_acquire) + 1;
                if (sleeping == thread_count && ctx.tasks_available.load(std::memory_order_acquire) == 0) {
                    // Everyone is asleep and no tasks! Deadlock/Stall detected!
                    // Since we ran topo_sort up front, this is not a cycle but a true scheduling bug (missed-notify).
#ifdef DEBUG
                    assert(false &&
                           "Invariant violated: Parallel executor stalled due to scheduler bug (missed-notify).");
#endif
                    ctx.tasks_available.store(std::numeric_limits<size_t>::max(), std::memory_order_release);
                    ctx.tasks_available.notify_all();
                    ctx.sleeping_threads.fetch_sub(1, std::memory_order_release);
                    return;
                }

                ctx.tasks_available.wait(curr, std::memory_order_acquire);
                ctx.sleeping_threads.fetch_sub(1, std::memory_order_release);
            }
        }

#if FF_cob__heterogeneous_core_affinity
        if (task.estimate > HEAVY_TASK_THRESHOLD) {
            setpriority(PRIO_PROCESS, 0, -5); // Hint: P-core
        } else {
            setpriority(PRIO_PROCESS, 0, 5); // Hint: E-core
        }
#endif

        int result = processStep(task.node_idx, ctx, node_times, is_tty);

#if FF_cob__heterogeneous_core_affinity
        setpriority(PRIO_PROCESS, 0, 0); // Reset to default
#endif

        if (result != 0) {
            ctx.error_occurred.store(true, std::memory_order_release);
            if (!config.keep_going) {
                ctx.completed_count.store(ctx.total_nodes, std::memory_order_release);
                ctx.tasks_available.store(std::numeric_limits<size_t>::max(), std::memory_order_release);
                ctx.tasks_available.notify_all(); // wake up everyone to exit
            } else {
                size_t prev_completed = ctx.completed_count.fetch_add(1, std::memory_order_release);
                if (prev_completed + 1 == ctx.total_nodes) {
                    ctx.tasks_available.store(std::numeric_limits<size_t>::max(), std::memory_order_release);
                    ctx.tasks_available.notify_all();
                }
            }
        } else {
            size_t prev_completed = ctx.completed_count.fetch_add(1, std::memory_order_release);
            const BuildGraph::Node &node = ctx.build_graph.nodes()[task.node_idx];
            for (size_t neighbor : node.out_edges) {
                if (ctx.in_degrees[neighbor].fetch_sub(1, std::memory_order_acq_rel) == 1) {
#if FF_cob__estimates
                    pushReady(neighbor, ctx, estimator.get());
#else
                    pushReady(neighbor, ctx, nullptr);
#endif
                }
            }
            if (prev_completed + 1 == ctx.total_nodes) {
                ctx.tasks_available.store(std::numeric_limits<size_t>::max(), std::memory_order_release);
                ctx.tasks_available.notify_all(); // wake up everyone to exit
            }
        }
    }
}

/**
 * @brief Executes the build.
 *
 * Initializes the execution context, resolves dependencies to identify initial ready tasks,
 * spawns worker threads, and coordinates the completion state of the build.
 *
 * @return Success or error.
 */
Result<void> Executor::execute() {
    pool.clear(); // Ensure clean state

    catalyst::BuildGraph build_graph = builder.emitGraph();

    // If graph is empty
    if (build_graph.nodes().empty())
        return {};

    // Run topo_sort once up front to detect real cycles deterministically.
    if (auto topo_res = build_graph.topoSort(); !topo_res) {
        return std::unexpected(topo_res.error());
    }

    const auto cc_vec = builder.getDefinitionOf<std::vector<std::string>>("cc");
    const auto cxx_vec = builder.getDefinitionOf<std::vector<std::string>>("cxx");
    const auto linker_vec = builder.getLinkerVec(cxx_vec);
    const auto archiver_vec = builder.getArchiverVec();

    const size_t total_nodes = build_graph.nodes().size();
    ExecuteContext ctx(total_nodes,
                       cc_vec,
                       cxx_vec,
                       linker_vec,
                       archiver_vec,
                       builder.getDefinitionOf<std::vector<std::string>>("cflags"),
                       builder.getDefinitionOf<std::vector<std::string>>("cxxflags"),
                       builder.getDefinitionOf<std::vector<std::string>>("ldflags"),
                       builder.getDefinitionOf<std::vector<std::string>>("ldlibs"),
                       std::move(build_graph));

    // Build initial in-degrees in a thread-safe atomic vector
    std::vector<int> temp_in_degrees(ctx.total_nodes, 0);
    for (const BuildGraph::Node &node : ctx.build_graph.nodes()) {
        for (size_t out : node.out_edges) {
            temp_in_degrees[out]++;
        }
    }
    for (size_t i = 0; i < ctx.in_degrees.size(); ++i) {
        ctx.in_degrees[i].store(temp_in_degrees[i], std::memory_order_relaxed);
    }

    // Pre-sort the initial root tasks to preserve Longest Processing Time first (LPT) scheduling
    std::vector<ExecuteContext::Task> initial_tasks;
    for (size_t i = 0; i < ctx.in_degrees.size(); ++i) {
        if (ctx.in_degrees[i].load(std::memory_order_relaxed) == 0) {
            size_t est = 0;
#if FF_cob__estimates
            const auto &node = ctx.build_graph.nodes()[i];
            if (node.step_id.has_value()) {
                est = estimator->getWorkEstimate(ctx.build_graph.steps()[*node.step_id].output);
            }
#endif
            initial_tasks.push_back({.node_idx = i, .estimate = est});
        }
    }
    std::ranges::sort(initial_tasks, [](const ExecuteContext::Task &a, const ExecuteContext::Task &b) {
        return a.estimate > b.estimate;
    });
    for (const ExecuteContext::Task &task : initial_tasks) {
#ifdef DEBUG
        bool enqueued = ctx.ready_queue.enqueue(task);
        assert(enqueued && "Invariant violated: ready_queue overflow during initialization");
#else
        ctx.ready_queue.enqueue(task);
#endif
        ctx.tasks_available.fetch_add(1, std::memory_order_release);
    }

    NodeTimes node_times(ctx.build_graph, config.build_file);
    bool is_tty = ::isatty(STDOUT_FILENO) != 0;

#if FF_cob__logging
    std::ofstream *log_file_ptr = nullptr;
    std::ofstream log_file;
    if (!config.build_log_file.empty()) {
        log_file.open(config.build_log_file, std::ios::trunc);
        if (!log_file) {
            return std::unexpected(std::format("Failed to open build log file: {}", config.build_log_file));
        }
        log_file_ptr = &log_file;
    }
#endif

    // Find final output target (cheap, no filesystem scan)
    std::string final_output_name;
    if (!config.silent) {
        for (size_t i = 0; i < ctx.total_nodes; ++i) {
            const BuildGraph::Node &node = ctx.build_graph.nodes()[i];
            if (node.step_id.has_value()) {
                const BuildStep &step = ctx.build_graph.steps()[*node.step_id];
                if (node.out_edges.empty()) {
                    final_output_name = step.output;
                }
            }
        }
    }

    size_t thread_count = config.jobs;
    if (thread_count == 0)
        thread_count = std::thread::hardware_concurrency();
    if (thread_count == 0)
        thread_count = 1;

    std::jthread progress_thread;
#if FF_cob__logging
    const bool start_progress = !config.silent || !config.build_log_file.empty();
#else
    const bool start_progress = !config.silent;
#endif
    if (start_progress) {
        progress_thread = std::jthread([&ctx,
                                        is_tty
#if FF_cob__logging
                                        ,
                                        log_file_ptr
#endif
        ]() {
            size_t processed = 0;
            while (true) {
                size_t enqueued = ctx.messages_enqueued.load(std::memory_order_acquire);
                while (processed == enqueued) {
                    ctx.messages_enqueued.wait(processed, std::memory_order_acquire);
                    enqueued = ctx.messages_enqueued.load(std::memory_order_acquire);
                }

                ExecuteContext::LogEvent ev;
                while (!ctx.progress_queue.dequeue(ev)) {
                    std::this_thread::yield();
                }
                processed++;

                if (ev.is_poison_pill) {
                    return;
                }
                if (ev.is_error) {
                    if (!ev.console_message.empty()) {
                        std::print(stderr, "{}", ev.console_message);
                        std::cerr.flush();
                    }
                } else {
                    if (!ev.console_message.empty()) {
                        std::print("{}", ev.console_message);
                        if (is_tty) {
                            std::cout.flush();
                        }
                    }
                }
#if FF_cob__logging
                if (!ev.log_message.empty() && log_file_ptr && log_file_ptr->is_open()) {
                    (*log_file_ptr) << ev.log_message;
                    if (ev.is_error || is_tty) {
                        log_file_ptr->flush();
                    }
                }
#endif
            }
        });
    }

    for (size_t i = 0; i < thread_count; ++i) {
        pool.emplace_back([this, &ctx, &node_times, is_tty, thread_count]() {
            this->workerLoop(ctx, node_times, is_tty, thread_count);
        });
    }

    pool.clear(); // Join all worker threads

    // Signal progress thread to exit
    if (start_progress) {
        while (!ctx.progress_queue.enqueue(
            {.console_message = "", .log_message = "", .is_error = false, .is_poison_pill = true}))
            std::this_thread::yield();
        ctx.messages_enqueued.fetch_add(1, std::memory_order_release);
        ctx.messages_enqueued.notify_one();
        progress_thread.join();
    }

    if (ctx.error_occurred) {
        if (!config.silent && is_tty && ctx.steps_completed > 0) {
            std::print("\n");
        }
        return std::unexpected("Build Failed");
    }

    if (ctx.completed_count != ctx.total_nodes) {
        if (!config.silent && is_tty && ctx.steps_completed > 0) {
            std::print("\n");
        }
#ifdef DEBUG
        assert(false && "Invariant violated: Build stalled due to runtime scheduler/concurrency bug.");
#endif
        return std::unexpected("Build stalled: Concurrency or scheduling bug detected.");
    }

    if (!config.silent && !final_output_name.empty()) {
        std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
        auto now_ns = std::chrono::time_point_cast<std::chrono::nanoseconds>(now);
        auto epoch_ns = now_ns.time_since_epoch().count();
        std::time_t now_time = std::chrono::system_clock::to_time_t(now);
        std::tm local_tm{};
        localtime_r(&now_time, &local_tm);
        constexpr int64_t NS_PER_SEC = 1'000'000'000;
        constexpr int TM_YEAR_BASE = 1900;
        constexpr size_t BUF_SIZE = 32;
        auto subsec_ns = epoch_ns % NS_PER_SEC;
        std::array<char, BUF_SIZE> buf{}; // 29 needed; round up for future to avoid dynamic allocation
        auto *end = std::format_to(buf.data(),
                                   "{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:09}",
                                   local_tm.tm_year + TM_YEAR_BASE,
                                   local_tm.tm_mon + 1,
                                   local_tm.tm_mday,
                                   local_tm.tm_hour,
                                   local_tm.tm_min,
                                   local_tm.tm_sec,
                                   subsec_ns);
        std::string_view timestamp(buf.data(), end - buf.data());
        if (is_tty) {
            std::println("\r\033[K[{}] \033[32m[COB FINISHED: {}]\033[0m", timestamp, final_output_name);
        } else {
            std::println("[{}] [COB FINISHED: {}]", timestamp, final_output_name);
        }
    }

    // Command hashes are the only graph state a build changes, so a no-op leaves the cache as it is.
    if (!config.dry_run && ctx.hashes_changed.load(std::memory_order_relaxed)) {
        builder.m_graph = std::move(ctx.build_graph);
        if (auto bin_res = emitBin(builder); !bin_res) {
            std::println(stderr, "Warning: Failed to write .catalyst.bin: {}", bin_res.error());
        }
    }

    return {};
}

} // namespace catalyst
