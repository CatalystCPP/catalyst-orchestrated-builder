#include "cbe/executor.hpp"

#include "cbe/builder.hpp"
#include "cbe/domain.hpp"
#include "cbe/process_exec.hpp"
#include "cbe/utility.hpp"
#include "nlohmann/json_fwd.hpp"

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
#include <mutex>
#include <nlohmann/json.hpp>
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

namespace {
struct BuildCompdbParams {
    const std::vector<size_t> &order;
    const catalyst::BuildGraph &build_graph;
    std::vector<std::string> cc_vec;
    std::vector<std::string> cxx_vec;
    std::vector<std::string> cflags_vec;
    std::vector<std::string> cxxflags_vec;
};

nlohmann::json buildCompdb(const BuildCompdbParams &params) {
    auto [order, build_graph, cc_vec, cxx_vec, cflags_vec, cxxflags_vec] = params;
    using JSON = nlohmann::json;
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
} // namespace

namespace catalyst {

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

    using JSON = nlohmann::json;
    JSON compdb = buildCompdb({
        .order = order,
        .build_graph = build_graph,
        .cc_vec = builder.getDefinitionOf<std::vector<std::string>>("cc"),
        .cxx_vec = builder.getDefinitionOf<std::vector<std::string>>("cxx"),
        .cflags_vec = builder.getDefinitionOf<std::vector<std::string>>("cflags"),
        .cxxflags_vec = builder.getDefinitionOf<std::vector<std::string>>("cxxflags"),
    });

    std::ofstream f("compile_commands.json");
    f << compdb.dump(4);
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

Result<void> Executor::execute() {
    pool.clear(); // Ensure clean state

    catalyst::BuildGraph build_graph = builder.emit_graph();

    const auto cc_vec = builder.getDefinitionOf<std::vector<std::string>>("cc");
    const auto cxx_vec = builder.getDefinitionOf<std::vector<std::string>>("cxx");
    const auto cflags_vec = builder.getDefinitionOf<std::vector<std::string>>("cflags");
    const auto cxxflags_vec = builder.getDefinitionOf<std::vector<std::string>>("cxxflags");
    const auto ldflags_vec = builder.getDefinitionOf<std::vector<std::string>>("ldflags");
    const auto ldlibs_vec = builder.getDefinitionOf<std::vector<std::string>>("ldlibs");

    // Build in-degrees
    std::vector<int> in_degrees(build_graph.nodes().size(), 0);
    for (const auto &node : build_graph.nodes()) {
        for (size_t out : node.out_edges) {
            in_degrees[out]++;
        }
    }

#if FF_cbe__heterogenous_core_affinity
    static constexpr size_t TUNABLE_HEAVY_THRESHOLD = 100;
#endif

    struct Task {
        size_t node_idx;
        size_t estimate;
        bool operator<(const Task &other) const {
            return estimate < other.estimate;
        }
    };
    std::priority_queue<Task> ready_queue;

    auto push_ready = [&](size_t idx) {
        size_t est = 0;
#if FF_cbe__estimates
        const auto &node = build_graph.nodes()[idx];
        if (node.step_id.has_value()) {
            est = estimator->getWorkEstimate(build_graph.steps()[*node.step_id].output);
        }
#endif
        ready_queue.push({.node_idx = idx, .estimate = est});
    };

    for (size_t i = 0; i < in_degrees.size(); ++i) {
        if (in_degrees[i] == 0) {
            push_ready(i);
        }
    }

    std::mutex mtx;
    std::condition_variable cv_ready;
    std::atomic<size_t> completed_count = 0;
    size_t total_nodes = build_graph.nodes().size();
    bool error_occurred = false;
    size_t active_workers = 0;

    StatCache stat_cache;

    std::mutex cout_tty_mtx;
    bool is_tty = ::isatty(STDOUT_FILENO) != 0;

#if FF_cbe__logging
    if (!config.build_log_file.empty()) {
        std::ofstream log_file(config.build_log_file, std::ios::trunc);
        if (!log_file) {
            return std::unexpected(std::format("Failed to open build log file: {}", config.build_log_file));
        }
    }
#endif

    // Pre-count steps that need rebuilding and find final output target
    size_t steps_to_build = 0;
    std::string final_output_name;
    for (size_t i = 0; i < build_graph.nodes().size(); ++i) {
        const auto &node = build_graph.nodes()[i];
        if (node.step_id.has_value()) {
            const auto &step = build_graph.steps()[*node.step_id];
            if (needs_rebuild(step, stat_cache)) {
                steps_to_build++;
            }
            if (node.out_edges.empty()) {
                final_output_name = step.output;
            }
        }
    }
    std::atomic<size_t> steps_completed = 0;

    // If graph is empty
    if (total_nodes == 0)
        return {};

    auto build_command_args = [&](const BuildStep &step, bool dry_run_mode) -> std::vector<std::string> {
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
            add_parts(ldflags_vec);
            add_parts(ldlibs_vec);
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

    auto print_message = [&](const BuildStep &step) {
        if (config.silent) {
            return;
        }

        std::lock_guard lock(cout_tty_mtx);

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

            // Pad the raw action name first, then wrap in color codes to ensure perfect TTY alignment
            std::string padded = std::format("{:<15}", raw_action);
            if (!color_code.empty()) {
                action = std::format("{}{}\033[0m", color_code, padded);
            } else {
                action = padded;
            }
        } else {
            action = raw_action;
        }

        if (config.dry_run) {
            std::cout << "[DRY RUN] " << action << " " << target << std::endl;
        } else {
            auto current = steps_completed.fetch_add(1, std::memory_order_relaxed) + 1;
            if (is_tty) {
                std::print("\r\033[K[{}/{}] {} {}", current, steps_to_build, action, target);
                std::cout.flush();
            } else {
                std::println("[{}/{}] {:<15} {}", current, steps_to_build, action, target);
            }
        }
    };

    auto process_step = [&](size_t node_idx) {
        // NOLINTBEGIN(performance-avoid-endl)
        const auto &node = build_graph.nodes()[node_idx];
        if (node.step_id.has_value()) {
            const auto &step = build_graph.steps()[*node.step_id];

            if (needs_rebuild(step, stat_cache)) {
                print_message(step);
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
                    std::lock_guard lock(cout_tty_mtx);
                    std::println("Step {} took {:.4f}s", step.output, diff.count());
                }
#endif

                if (res) {
                    auto [ec, output] = *res;
#if FF_cbe__logging
                    if (capture_output && !output.empty()) {
                        std::lock_guard lock(cout_tty_mtx);
                        if (!config.silent || ec != 0) {
                            std::print("{}", output);
                        }

                        std::ofstream log_file(config.build_log_file, std::ios_base::app);
                        if (log_file) {
                            log_file << "=== " << step.tool << " -> " << step.output << " ===\n";
                            log_file << output;
                            if (output.back() != '\n') {
                                log_file << '\n';
                            }
                            log_file << '\n';
                        }
                    }
#endif
                    if (ec != 0) {
                        std::println(stderr, "Build failed: {} -> {} (exit code {})", step.tool, step.output, ec);
                        return ec;
                    }
                } else {
                    std::println(stderr, "Failed to execute: {}", res.error());
                    return 1;
                }
            }
#if FF_cbe__logging
            else {
                std::lock_guard lock(cout_tty_mtx);
                std::println("Skipping {} (up to date)", step.output);
            }
#endif
        }
        return 0;
        // NOLINTEND(performance-avoid-endl)
    };

    auto worker = [&]() {
        while (true) {
            Task task;
            {
                std::unique_lock lock(mtx);
                cv_ready.wait(lock, [&] {
                    return !ready_queue.empty() || completed_count == total_nodes || active_workers == 0; //
                });

                if (ready_queue.empty()) {
                    if (completed_count == total_nodes)
                        return;
                    // If queue is empty and no active workers, but work not done -> Cycle
                    return;
                }

                task = ready_queue.top();
                ready_queue.pop();
                active_workers++;
            }

#if FF_cbe__heterogenous_core_affinity
            if (task.estimate > TUNABLE_HEAVY_THRESHOLD) {
                setpriority(PRIO_PROCESS, 0, -5); // Hint: P-core
            } else {
                setpriority(PRIO_PROCESS, 0, 5);  // Hint: E-core
            }
#endif

            int result = process_step(task.node_idx);

#if FF_cbe__heterogenous_core_affinity
            setpriority(PRIO_PROCESS, 0, 0); // Reset to default
#endif

            {
                std::lock_guard lock(mtx);
                active_workers--;

                size_t new_work_count = 0;

                if (result != 0) {
                    error_occurred = true;
                    if (!config.keep_going) {
                        completed_count = total_nodes; // Force exit
                    } else {
                        completed_count.fetch_add(1, std::memory_order_relaxed);
                    }
                } else {
                    completed_count.fetch_add(1, std::memory_order_relaxed);
                    const auto &node = build_graph.nodes()[task.node_idx];
                    for (size_t neighbor : node.out_edges) {
                        in_degrees[neighbor]--;
                        if (in_degrees[neighbor] == 0) {
                            push_ready(neighbor);
                            new_work_count++;
                        }
                    }
                }

                bool build_finished = (completed_count == total_nodes);
                bool stall_detected = (active_workers == 0);
                constexpr auto TUNABLE__notify_all_criteria = 10;
                if (build_finished || error_occurred || stall_detected ||
                    new_work_count >= TUNABLE__notify_all_criteria) {
                    cv_ready.notify_all();
                } else if (new_work_count == 1) {
                    cv_ready.notify_one();
                } else {
                    for (size_t ii = 0; ii < new_work_count; ++ii) {
                        cv_ready.notify_one();
                    }
                }
            }
        }
    };

    size_t thread_count = config.jobs;
    if (thread_count == 0)
        thread_count = std::thread::hardware_concurrency();
    if (thread_count == 0)
        thread_count = 1;

    for (size_t i = 0; i < thread_count; ++i) {
        pool.emplace_back(worker);
    }

    pool.clear(); // Join all threads

    if (error_occurred) {
        if (!config.silent && is_tty && steps_completed > 0) {
            std::print("\n");
        }
        return std::unexpected("Build Failed");
    }

    if (completed_count != total_nodes) {
        if (!config.silent && is_tty && steps_completed > 0) {
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
