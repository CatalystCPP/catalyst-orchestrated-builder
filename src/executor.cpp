#include "cbe/executor.hpp"

#include "cbe/builder.hpp"
#include "cbe/build_step.hpp"
#include "cbe/json.hpp"
#include "cbe/mpmc_queue.hpp"
#include "cbe/mpsc_queue.hpp"
#include "cbe/process_exec.hpp"
#include "cbe/utility.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <ostream>
#include <print>
#include <queue>
#include <ranges>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>

#if FF_cbe__heterogenous_core_affinity
#include <sys/resource.h>
#endif

namespace catalyst {
struct BuildCompdbParams {
    const std::vector<size_t> &order;
    const catalyst::BuildGraph &build_graph;
    std::vector<std::string> cc_vec;
    std::vector<std::string> cxx_vec;
    std::vector<std::string> cflags_vec;
    std::vector<std::string> cxxflags_vec;
};

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
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    res.append(buf);
                } else {
                    res.push_back(c);
                }
                break;
        }
    }
    res.push_back('"');
    return res;
}

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

void writeCompdb(const catalyst::JSON &compdb, std::ofstream &out) {
    dumpJSON(compdb, out);
}

catalyst::JSON buildCompdb(const BuildCompdbParams &params) {
    auto [order, build_graph, cc_vec, cxx_vec, cflags_vec, cxxflags_vec] = params;
    using JSON = catalyst::JSON;
    JSON compdb = JSON::array();
    auto cwd = std::filesystem::current_path().string();

    for (size_t node_idx : order) {
        const auto &node = build_graph.nodes()[node_idx];
        if (!node.step_id.has_value())
            continue;
        const auto &step = build_graph.steps()[*node.step_id];

        // Only emit for compilation steps
        if (step.tool != "cc" && step.tool != "cxx")
            continue;

        const std::vector<std::string_view> &inputs = step.parsed_inputs;

        std::vector<std::string> args;
        auto add_parts = [&args](const auto &parts) {
            for (const auto &part : parts) {
                if (part.begin() != part.end()) {
                    args.push_back(std::ranges::to<std::string>(part));
                }
            }
        };

        constexpr static size_t EXTRA_ARGS_RESERVED_SPACE = 7;
        if (step.tool == "cc") {
            add_parts(cc_vec);
            add_parts(cflags_vec);
            args.reserve(args.size() + inputs.size() + EXTRA_ARGS_RESERVED_SPACE);
            args.insert(args.end(), {"-MMD", "-MF", std::string(step.output) + ".d", "-c"});
            for (const auto &in : inputs)
                args.emplace_back(in);
            args.emplace_back("-o");
            args.emplace_back(step.output);
        } else if (step.tool == "cxx") {
            add_parts(cxx_vec);
            add_parts(cxxflags_vec);
            args.reserve(args.size() + inputs.size() + EXTRA_ARGS_RESERVED_SPACE);
            args.insert(args.end(), {"-MMD", "-MF", std::string(step.output) + ".d", "-c"});
            args.reserve(args.size() + inputs.size());
            for (const auto &in : inputs)
                args.emplace_back(in);
            args.emplace_back("-o");
            args.emplace_back(step.output);
        }

        JSON entry;
        entry["directory"] = cwd;
        entry["arguments"] = args;
        if (!inputs.empty()) {
            entry["file"] = inputs[0];
        }
        entry["output"] = step.output;
        compdb.push_back(entry);
    }

    return compdb;
}

bool isNewer(const std::filesystem::path &new_file, const std::filesystem::path &old_file) {
    std::error_code ec;
    auto new_time = std::filesystem::last_write_time(new_file, ec);
    if (ec)
        return true;
    auto old_time = std::filesystem::last_write_time(old_file, ec);
    if (ec)
        return true;
    return new_time > old_time;
}

Executor::Executor(CBEBuilder &&builder, const ExecutorConfig &config) : builder(std::move(builder)), config(config) {
#if FF_cbe__estimates
    estimator = std::make_unique<WorkEstimate>(config.estimates_file);
#endif
}

Result<void> Executor::clean() {
    catalyst::BuildGraph build_graph = builder.emit_graph();
    std::println("Cleaning build artifacts...");

    for (const auto &step : build_graph.steps()) {
        if (config.clean_cc_only)
            if (step.tool != "cxx" && step.tool != "cc") [[likely]]
                continue;

        if (std::filesystem::exists(step.output)) {
            std::error_code ec;
            std::filesystem::remove(step.output, ec);
            if (ec) {
                std::println(stderr, "Failed to remove {}: {}", step.output, ec.message());
            } else {
                std::println("Removed {}", step.output);
            }
        }
        // Also clean .d files if they exist
        auto d_file = std::string(step.output) + ".d";
        if (std::filesystem::exists(d_file)) {
            std::filesystem::remove(d_file);
        }
    }
    return {};
}

[[clang::always_inline]]
bool inline Executor::needs_rebuild(const BuildStep &step, StatCache &stat_cache) const {
    if (!std::filesystem::exists(step.output))
        return true;

    auto output_modtime = std::filesystem::last_write_time(step.output);

    if (stat_cache.changed_since(config.build_file, output_modtime)) {
        return true;
    }

    if (step.depfile_inputs.has_value()) {
        for (const auto &dep : *step.depfile_inputs) {
            if (stat_cache.changed_since(std::filesystem::path(dep), output_modtime)) {
                return true;
            }
        }
    }
    if (step.opaque_inputs.has_value()) {
        for (const auto &opaque : *step.opaque_inputs) {
            if (stat_cache.changed_since(std::filesystem::path(opaque), output_modtime)) {
                return true;
            }
        }
    }
    // this is our way of making sure that the .d file isn't stale
    for (const auto &input : step.parsed_inputs) {
        if (stat_cache.changed_since(input, output_modtime)) {
            return true;
        }
    }
    return false;
}

Result<void> Executor::emit_graph() {
    catalyst::BuildGraph build_graph = builder.emit_graph();
    StatCache stat_cache;

    std::cout << "digraph catalyst_build {\n";
    std::cout << "  rankdir=LR;\n";
    std::cout << "  node [shape=box, style=filled, fontname=\"Helvetica\"];\n";

    for (size_t i = 0; i < build_graph.nodes().size(); ++i) {
        const auto &node = build_graph.nodes()[i];
        std::string color = "0.9 0.9 0.9"; // light gray for source files

        if (node.step_id.has_value()) {
            const auto &step = build_graph.steps()[*node.step_id];
            if (needs_rebuild(step, stat_cache)) {
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

Result<void> Executor::emit_compdb() {
    catalyst::BuildGraph build_graph = builder.emit_graph();
    std::vector<size_t> order;
    auto res = build_graph.topo_sort();
    if (!res)
        return std::unexpected(res.error());
    order = *res;

    catalyst::JSON compdb = buildCompdb({
        .order = order,
        .build_graph = build_graph,
        .cc_vec = builder.getDefinitionOf<std::vector<std::string>>("cc"),
        .cxx_vec = builder.getDefinitionOf<std::vector<std::string>>("cxx"),
        .cflags_vec = builder.getDefinitionOf<std::vector<std::string>>("cflags"),
        .cxxflags_vec = builder.getDefinitionOf<std::vector<std::string>>("cxxflags"),
    });

    std::ofstream f("compile_commands.json");
    writeCompdb(compdb, f);
    return {};
}

Result<void> Executor::emit_commands() {
    catalyst::BuildGraph build_graph = builder.emit_graph();
    std::vector<size_t> order;
    auto res = build_graph.topo_sort();
    if (!res)
        return std::unexpected(res.error());
    order = *res;

    const auto cc_vec = builder.getDefinitionOf<std::vector<std::string>>("cc");
    const auto cxx_vec = builder.getDefinitionOf<std::vector<std::string>>("cxx");
    const auto cflags_vec = builder.getDefinitionOf<std::vector<std::string>>("cflags");
    const auto cxxflags_vec = builder.getDefinitionOf<std::vector<std::string>>("cxxflags");

    auto build_command_args_internal = [&](const BuildStep &step) -> std::vector<std::string> {
        std::vector<std::string> args;
        auto add_parts = [&args](const auto &parts) {
            for (const auto &part : parts) {
                if (part.begin() != part.end()) {
                    args.push_back(std::ranges::to<std::string>(part));
                }
            }
        };

        const auto &inputs = step.parsed_inputs;

        if (step.tool == "cc") {
            add_parts(cc_vec);
            add_parts(cflags_vec);
            args.insert(args.end(), {"-MMD", "-MF", std::string(step.output) + ".d", "-c"});
            for (const auto &in : inputs)
                args.emplace_back(in);
            args.emplace_back("-o");
            args.emplace_back(step.output);
        } else if (step.tool == "cxx") {
            add_parts(cxx_vec);
            add_parts(cxxflags_vec);
            args.insert(args.end(), {"-MMD", "-MF", std::string(step.output) + ".d", "-c"});
            for (const auto &in : inputs)
                args.emplace_back(in);
            args.emplace_back("-o");
            args.emplace_back(step.output);
        } else if (step.tool == "ld") {
            add_parts(cxx_vec);
            for (const auto &in : inputs)
                args.emplace_back(in);
            args.emplace_back("-o");
            args.emplace_back(step.output);
        } else if (step.tool == "ar") {
            args.insert(args.end(), {"ar", "rcs", std::string(step.output)});
            for (const auto &in : inputs)
                args.emplace_back(in);
        } else if (step.tool == "sld") {
            add_parts(cxx_vec);
            args.emplace_back("-shared");
            for (const auto &in : inputs)
                args.emplace_back(in);
            args.emplace_back("-o");
            args.emplace_back(step.output);
        }
        return args;
    };

    for (size_t node_idx : order) {
        const auto &node = build_graph.nodes()[node_idx];
        if (!node.step_id.has_value())
            continue;
        const auto &step = build_graph.steps()[*node.step_id];
        auto args = build_command_args_internal(step);
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
    std::atomic<size_t> tasks_available{0};
    std::atomic<size_t> sleeping_threads{0};

    LockFreeMPSCQueue<LogEvent> progress_queue;
    std::atomic<size_t> messages_enqueued{0};

    std::vector<std::atomic<int>> in_degrees;

    std::atomic<size_t> completed_count{0};
    std::atomic<size_t> steps_completed{0};
    size_t total_nodes{0};
    size_t steps_to_build{0};
    std::atomic<bool> error_occurred{false};

    const std::vector<std::string> cc_vec;
    const std::vector<std::string> cxx_vec;
    const std::vector<std::string> cflags_vec;
    const std::vector<std::string> cxxflags_vec;
    const std::vector<std::string> ldflags_vec;
    const std::vector<std::string> ldlibs_vec;

    catalyst::BuildGraph build_graph;

    ExecuteContext(size_t node_count,
                   std::vector<std::string> cc,
                   std::vector<std::string> cxx,
                   std::vector<std::string> cflags,
                   std::vector<std::string> cxxflags,
                   std::vector<std::string> ldflags,
                   std::vector<std::string> ldlibs,
                   catalyst::BuildGraph bg)
        : ready_queue(node_count), progress_queue(8192), in_degrees(node_count), total_nodes(node_count),
          cc_vec(std::move(cc)), cxx_vec(std::move(cxx)), cflags_vec(std::move(cflags)),
          cxxflags_vec(std::move(cxxflags)), ldflags_vec(std::move(ldflags)), ldlibs_vec(std::move(ldlibs)),
          build_graph(std::move(bg)) {
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
std::vector<std::string> Executor::build_command_args(const BuildStep &step, bool dry_run_mode) const {
    std::vector<std::string> args;
    static constexpr auto ARGS_VEC_INIT_SZ = 40;
    args.reserve(ARGS_VEC_INIT_SZ);

    auto add_parts = [&args](const auto &parts) {
        args.reserve(args.size() + parts.size());
        for (const auto &part : parts) {
            if (part.begin() != part.end()) {
                args.push_back(std::ranges::to<std::string>(part));
            }
        }
    };

    const auto &inputs = step.parsed_inputs;

    if (step.tool == "cc") {
        add_parts(builder.getDefinitionOf<std::vector<std::string>>("cc"));
        add_parts(builder.getDefinitionOf<std::vector<std::string>>("cflags"));
        args.insert(args.end(), {"-MMD", "-MF", std::string(step.output) + ".d", "-c"});
        for (const auto &in : inputs)
            args.emplace_back(in);
        args.emplace_back("-o");
        args.emplace_back(step.output);
    } else if (step.tool == "cxx") {
        add_parts(builder.getDefinitionOf<std::vector<std::string>>("cxx"));
        add_parts(builder.getDefinitionOf<std::vector<std::string>>("cxxflags"));
        args.insert(args.end(), {"-MMD", "-MF", std::string(step.output) + ".d", "-c"});
        for (const auto &in : inputs)
            args.emplace_back(in);
        args.emplace_back("-o");
        args.emplace_back(step.output);
    } else if (step.tool == "ld") {
        add_parts(builder.getDefinitionOf<std::vector<std::string>>("cxx"));
        static constexpr auto TUNABLE_INPUT_SZ = 50;
        std::filesystem::path rsp_path = std::filesystem::path(step.output).replace_extension(".rsp");

        bool use_rsp = false;
        if (std::filesystem::exists(rsp_path) && isNewer(rsp_path, config.build_file)) {
            use_rsp = true;
        } else if (inputs.size() > TUNABLE_INPUT_SZ) {
            use_rsp = true;
            if (!dry_run_mode) {
                std::string rsp_content;
                constexpr auto TUNABLE_RSP_PATH_ESTIMATE = 100;
                rsp_content.reserve(inputs.size() * TUNABLE_RSP_PATH_ESTIMATE);
                for (const auto &input : inputs) {
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
            for (const auto &in : inputs)
                args.emplace_back(in);
        }
        args.emplace_back("-o");
        args.emplace_back(step.output);
        add_parts(builder.getDefinitionOf<std::vector<std::string>>("ldflags"));
        add_parts(builder.getDefinitionOf<std::vector<std::string>>("ldlibs"));
    } else if (step.tool == "ar") {
        args.insert(args.end(), {"ar", "rcs", std::string(step.output)});
        for (const auto &in : inputs)
            args.emplace_back(in);
    } else if (step.tool == "sld") {
        add_parts(builder.getDefinitionOf<std::vector<std::string>>("cxx"));
        args.emplace_back("-shared");
        for (const auto &in : inputs)
            args.emplace_back(in);
        args.emplace_back("-o");
        args.emplace_back(step.output);
    }
    return args;
}

/**
 * @brief Enqueues a task representing a node ready to be processed.
 *
 * If work estimates are enabled, calculates the estimate for the node and assigns it.
 * Utilizes the lock-free queue and notifies a sleeping worker thread via atomic wait primitives.
 */
void Executor::push_ready(size_t idx, ExecuteContext &ctx) {
    size_t est = 0;
#if FF_cbe__estimates
    const auto &node = ctx.build_graph.nodes()[idx];
    if (node.step_id.has_value()) {
        est = estimator->getWorkEstimate(ctx.build_graph.steps()[*node.step_id].output);
    }
#endif
    ctx.ready_queue.enqueue({.node_idx = idx, .estimate = est});
    ctx.tasks_available.fetch_add(1, std::memory_order_release);
    ctx.tasks_available.notify_one();
}

/**
 * @brief Formats and prints a thread-safe progress message for a build step.
 *
 * Uses terminal color codes if stdout is a TTY and updates the line dynamically.
 */
void Executor::print_message(const BuildStep &step, ExecuteContext &ctx, bool is_tty) const {
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
        auto current = ctx.steps_completed.fetch_add(1, std::memory_order_relaxed) + 1;
        if (is_tty) {
            msg = std::format("\r\033[K[{}/{}] {} {}", current, ctx.steps_to_build, action, target);
        } else {
            msg = std::format("[{}/{}] {:<15} {}\n", current, ctx.steps_to_build, action, target);
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
int Executor::process_step(size_t node_idx, ExecuteContext &ctx, StatCache &stat_cache, bool is_tty) const {
    // NOLINTBEGIN(performance-avoid-endl)
    const auto &node = ctx.build_graph.nodes()[node_idx];
    if (node.step_id.has_value()) {
        const auto &step = ctx.build_graph.steps()[*node.step_id];

        if (needs_rebuild(step, stat_cache)) {
            print_message(step, ctx, is_tty);
            if (config.dry_run)
                return 0;

            auto args = build_command_args(step, false);

#if FF_cbe__profiling
            auto start = std::chrono::steady_clock::now();
#endif
#if FF_cbe__logging
            bool capture_output = !config.build_log_file.empty();
#else
            bool capture_output = false;
#endif
            auto res = catalyst::process_exec(std::move(args), std::nullopt, std::nullopt, capture_output);
#if FF_cbe__profiling
            auto end = std::chrono::steady_clock::now();
            std::chrono::duration<double> diff = end - start;
            {
                std::string msg = std::format("Step {} took {:.4f}s\n", step.output, diff.count());
                while (!ctx.progress_queue.enqueue({msg, "", false, false}))
                    std::this_thread::yield();
                ctx.messages_enqueued.fetch_add(1, std::memory_order_release);
                ctx.messages_enqueued.notify_one();
            }
#endif

            if (res) {
                auto [ec, output] = *res;
#if FF_cbe__logging
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
                    while (!ctx.progress_queue.enqueue(
                        {.console_message = err_msg, .log_message = "", .is_error = true, .is_poison_pill = false}))
                        std::this_thread::yield();
                    ctx.messages_enqueued.fetch_add(1, std::memory_order_release);
                    ctx.messages_enqueued.notify_one();
                    return ec;
                }
            } else {
                std::string err_msg = std::format("Failed to execute: {}\n", res.error());
                while (!ctx.progress_queue.enqueue(
                    {.console_message = err_msg, .log_message = "", .is_error = true, .is_poison_pill = false}))
                    std::this_thread::yield();
                ctx.messages_enqueued.fetch_add(1, std::memory_order_release);
                ctx.messages_enqueued.notify_one();
                return 1;
            }
        }
#if FF_cbe__logging
        else {
            std::string msg = std::format("Skipping {} (up to date)\n", step.output);
            while (!ctx.progress_queue.enqueue({msg, "", false, false}))
                std::this_thread::yield();
            ctx.messages_enqueued.fetch_add(1, std::memory_order_release);
            ctx.messages_enqueued.notify_one();
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
void Executor::worker_loop(ExecuteContext &ctx, StatCache &stat_cache, bool is_tty, size_t thread_count) {
#if FF_cbe__heterogenous_core_affinity
    static constexpr size_t TUNABLE_HEAVY_THRESHOLD = 100;
#endif

    while (true) {
        ExecuteContext::Task task;
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
                    // Or a normal completion race condition where notify_all was missed.
                    ctx.tasks_available.store(std::numeric_limits<size_t>::max(), std::memory_order_release);
                    ctx.tasks_available.notify_all();
                    ctx.sleeping_threads.fetch_sub(1, std::memory_order_release);
                    return;
                }

                ctx.tasks_available.wait(curr, std::memory_order_acquire);
                ctx.sleeping_threads.fetch_sub(1, std::memory_order_release);
            }
        }

#if FF_cbe__heterogenous_core_affinity
        if (task.estimate > TUNABLE_HEAVY_THRESHOLD) {
            setpriority(PRIO_PROCESS, 0, -5); // Hint: P-core
        } else {
            setpriority(PRIO_PROCESS, 0, 5); // Hint: E-core
        }
#endif

        int result = process_step(task.node_idx, ctx, stat_cache, is_tty);

#if FF_cbe__heterogenous_core_affinity
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
            const auto &node = ctx.build_graph.nodes()[task.node_idx];
            for (size_t neighbor : node.out_edges) {
                if (ctx.in_degrees[neighbor].fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    push_ready(neighbor, ctx);
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

    catalyst::BuildGraph build_graph = builder.emit_graph();

    // If graph is empty
    if (build_graph.nodes().empty())
        return {};

    ExecuteContext ctx(build_graph.nodes().size(),
                       builder.getDefinitionOf<std::vector<std::string>>("cc"),
                       builder.getDefinitionOf<std::vector<std::string>>("cxx"),
                       builder.getDefinitionOf<std::vector<std::string>>("cflags"),
                       builder.getDefinitionOf<std::vector<std::string>>("cxxflags"),
                       builder.getDefinitionOf<std::vector<std::string>>("ldflags"),
                       builder.getDefinitionOf<std::vector<std::string>>("ldlibs"),
                       std::move(build_graph));

    // Build initial in-degrees in a thread-safe atomic vector
    std::vector<int> temp_in_degrees(ctx.total_nodes, 0);
    for (const auto &node : ctx.build_graph.nodes()) {
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
#if FF_cbe__estimates
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
    for (const auto &task : initial_tasks) {
        ctx.ready_queue.enqueue(task);
        ctx.tasks_available.fetch_add(1, std::memory_order_release);
    }

    StatCache stat_cache;
    bool is_tty = ::isatty(STDOUT_FILENO) != 0;

#if FF_cbe__logging
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

    // Pre-count steps that need rebuilding and find final output target
    std::string final_output_name;
    for (size_t i = 0; i < ctx.total_nodes; ++i) {
        const auto &node = ctx.build_graph.nodes()[i];
        if (node.step_id.has_value()) {
            const auto &step = ctx.build_graph.steps()[*node.step_id];
            if (needs_rebuild(step, stat_cache)) {
                ctx.steps_to_build++;
            }
            if (node.out_edges.empty()) {
                final_output_name = step.output;
            }
        }
    }

    size_t thread_count = config.jobs;
    if (thread_count == 0)
        thread_count = std::thread::hardware_concurrency();
    if (thread_count == 0)
        thread_count = 1;

    std::jthread progress_thread([&ctx
#if FF_cbe__logging
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
                    std::cout.flush();
                }
            }
#if FF_cbe__logging
            if (!ev.log_message.empty() && log_file_ptr && log_file_ptr->is_open()) {
                (*log_file_ptr) << ev.log_message;
                log_file_ptr->flush();
            }
#endif
        }
    });

    for (size_t i = 0; i < thread_count; ++i) {
        pool.emplace_back([this, &ctx, &stat_cache, is_tty, thread_count]() {
            this->worker_loop(ctx, stat_cache, is_tty, thread_count);
        });
    }

    pool.clear(); // Join all worker threads

    // Signal progress thread to exit
    while (!ctx.progress_queue.enqueue({"", "", false, true}))
        std::this_thread::yield();
    ctx.messages_enqueued.fetch_add(1, std::memory_order_release);
    ctx.messages_enqueued.notify_one();
    progress_thread.join();

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
        return std::unexpected("Cycle detected: Build stalled with pending nodes.");
    }

    if (!config.silent && !final_output_name.empty()) {
        auto now = std::chrono::system_clock::now();
        auto now_ns = std::chrono::time_point_cast<std::chrono::nanoseconds>(now);
        auto epoch_ns = now_ns.time_since_epoch().count();
        std::time_t now_time = std::chrono::system_clock::to_time_t(now);
        std::tm local_tm{};
        localtime_r(&now_time, &local_tm);
        auto subsec_ns = epoch_ns % 1'000'000'000;
        auto timestamp = std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:09}",
                                     local_tm.tm_year + 1900,
                                     local_tm.tm_mon + 1,
                                     local_tm.tm_mday,
                                     local_tm.tm_hour,
                                     local_tm.tm_min,
                                     local_tm.tm_sec,
                                     subsec_ns);
        if (is_tty) {
            std::println("\r\033[K[{}] \033[32m[CBE FINISHED: {}]\033[0m", timestamp, final_output_name);
        } else {
            std::println("[{}] [CBE FINISHED: {}]", timestamp, final_output_name);
        }
    }

    return {};
}

} // namespace catalyst
