#include "tests/test_suite.hpp"
#include "tests/testing_utils.hpp"

#include "cbe/binary.hpp"
#include "cbe/builder.hpp"
#include "cbe/executor.hpp"

#include <cassert>
#include <filesystem>
#include <iostream>
#include <print>

using namespace catalyst;

bool rebuild_command_change_test() {
    std::println("Starting Command-Line/Flag Change Rebuild Test...");

    // Clean up any existing .catalyst.bin
    if (std::filesystem::exists(".catalyst.bin")) {
        std::filesystem::remove(".catalyst.bin");
    }

    create_dummy_file("catalyst.build");
    // Set catalyst.build mtime to 1 hour in the past to ensure output files are newer than it
    std::filesystem::last_write_time("catalyst.build", std::filesystem::last_write_time("catalyst.build") - std::chrono::hours(1));

    create_dummy_file("dummy_rebuild.c");

    // 1. Initial Build with -DTEST1
    {
        CBEBuilder builder;
        builder.add_definition("cc", "clang");
        builder.add_definition("cflags", "-DTEST1");

        BuildStep step;
        step.tool = "cc";
        step.inputs = "dummy_rebuild.c";
        step.output = "dummy_rebuild.o";

        auto res = builder.add_step(std::move(step));
        if (!res) {
            std::println(std::cerr, "Failed to add step in run 1: {}", res.error());
            return false;
        }

        Executor executor(std::move(builder), ExecutorConfig{});
        auto exec_res = executor.execute();
        if (!exec_res) {
            std::println(std::cerr, "Run 1 execution failed: {}", exec_res.error());
            return false;
        }
    }

    // 2. Second Build with SAME flags (-DTEST1). Should skip!
    {
        CBEBuilder builder;
        std::cout << "[Run 2] parsing bin..." << std::endl;
        auto parse_res = parse_bin(builder);
        if (!parse_res) {
            std::println(std::cerr, "Failed to parse .catalyst.bin in run 2: {}", parse_res.error());
            return false;
        }

        builder.add_definition("cflags", "-DTEST1");

        std::cout << "[Run 2] creating executor..." << std::endl;
        Executor executor(std::move(builder), ExecutorConfig{});
        std::cout << "[Run 2] executing..." << std::endl;
        auto exec_res = executor.execute();
        if (!exec_res) {
            std::println(std::cerr, "Run 2 execution failed: {}", exec_res.error());
            return false;
        }
        std::cout << "[Run 2] execution finished" << std::endl;
    }
    std::cout << "[Run 2] block exited" << std::endl;

    // 3. Third Build with DIFFERENT flags (-DTEST2). Should rebuild!
    {
        CBEBuilder builder;
        std::cout << "[Run 3] parsing bin..." << std::endl;
        auto parse_res = parse_bin(builder);
        if (!parse_res) {
            std::println(std::cerr, "Failed to parse .catalyst.bin in run 3: {}", parse_res.error());
            return false;
        }

        builder.add_definition("cflags", "-DTEST2");

        std::cout << "[Run 3] creating executor..." << std::endl;
        Executor executor(std::move(builder), ExecutorConfig{});
        std::cout << "[Run 3] executing..." << std::endl;
        auto exec_res = executor.execute();
        if (!exec_res) {
            std::println(std::cerr, "Run 3 execution failed: {}", exec_res.error());
            return false;
        }
        std::cout << "[Run 3] execution finished" << std::endl;
    }

    // Cleanup
    if (std::filesystem::exists("dummy_rebuild.c"))
        std::filesystem::remove("dummy_rebuild.c");
    if (std::filesystem::exists("dummy_rebuild.o"))
        std::filesystem::remove("dummy_rebuild.o");
    if (std::filesystem::exists("dummy_rebuild.o.d"))
        std::filesystem::remove("dummy_rebuild.o.d");
    if (std::filesystem::exists("catalyst.build"))
        std::filesystem::remove("catalyst.build");
    if (std::filesystem::exists(".catalyst.bin"))
        std::filesystem::remove(".catalyst.bin");

    std::println("Command-Line/Flag Change Rebuild Test passed!");
    return true;
}

bool integration_test() {
    // Setup
    std::println("Starting Integration Test...");
    create_dummy_file("catalyst.build");
    std::filesystem::last_write_time("catalyst.build", std::filesystem::last_write_time("catalyst.build") - std::chrono::hours(1));

    create_dummy_file("dummy.c");

    CBEBuilder builder;
    builder.add_definition("cc", "clang"); // Mock cc with echo
    builder.add_definition("cflags", "-DTEST");

    BuildStep step;
    step.tool = "cc";
    step.inputs = "dummy.c";
    step.output = "dummy.o";

    // Add step
    auto res = builder.add_step(std::move(step));
    if (!res) {
        std::println(std::cerr, "Failed to add step: {}", res.error());
        return false;
    }

    Executor executor(std::move(builder), ExecutorConfig{});
    auto exec_res = executor.execute();

    if (!exec_res) {
        std::println(std::cerr, "Execution failed: {}", exec_res.error());
        return false;
    }

    std::println("Test passed!");

    // Cleanup
    if (std::filesystem::exists("dummy.c"))
        std::filesystem::remove("dummy.c");
    if (std::filesystem::exists("dummy.o"))
        std::filesystem::remove("dummy.o");
    if (std::filesystem::exists("dummy.o.d"))
        std::filesystem::remove("dummy.o.d");
    if (std::filesystem::exists("catalyst.build"))
        std::filesystem::remove("catalyst.build");

    // Run command line/flag change rebuild test
    if (!rebuild_command_change_test()) {
        return false;
    }

    return true;
}
