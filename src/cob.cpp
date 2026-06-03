#include "cob/builder.hpp"
#include "cob/cli_args.hpp"
#include "cob/executor.hpp"
#include "cob/parser.hpp"

#include <filesystem>
#include <format>
#include <iostream>
#include <print>
#include <string_view>
#include <utility>

int main(const int argc, const char *const *argv) {
    auto cli_args_res = catalyst::cliArgs(argc, argv);

    if (!cli_args_res) {
        if (cli_args_res.error() != "") {
            std::cerr << cli_args_res.error() << '\n';
            return 1;
        }
        return 0;
    }

    const auto [config, compdb, graph, commands, work_dir, definition_overrides] = *cli_args_res;

    if (work_dir != ".") {
        std::error_code ec;
        std::filesystem::current_path(work_dir, ec);
        if (ec) {
            std::println(std::cerr, "Failed to change directory to {}: {}", work_dir.string(), ec.message());
            return 1;
        }
    }

    catalyst::COBBuilder builder;

    if (!std::filesystem::exists(config.build_file)) {
        std::println(std::cerr, "Build File: {} does not exist.", config.build_file);
        return 1;
    }

    if (auto parse_res = catalyst::parse(builder, config.build_file); !parse_res) {
        std::println(std::cerr, "Failed to parse: {}", parse_res.error());
        return 1;
    }

    for (const auto &[variable, value] : definition_overrides) {
        builder.add_definition(variable, value);
    }

    catalyst::Executor executor{std::move(builder), config};

    if (compdb) {
        auto _ = executor.emitCompDB();
    } else if (graph) {
        auto _ = executor.emitGraph();
    } else if (commands) {
        auto _ = executor.emitCommands();
    } else if (config.clean) {
        if (auto executor_clean_res = executor.clean(); !executor_clean_res) {
            std::println(std::cerr, "Clean failed: {}", executor_clean_res.error());
            return 1;
        }
    } else if (auto executor_execute_res = executor.execute(); !executor_execute_res) {
        std::println(std::cerr, "Execution failed: {}", executor_execute_res.error());
        return 1;
    }

    return 0;
}
