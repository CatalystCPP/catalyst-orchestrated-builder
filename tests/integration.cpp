#include "tests/test_suite.hpp"
#include "tests/testing_utils.hpp"

#include "cbe/builder.hpp"
#include "cbe/executor.hpp"

#include <cassert>
#include <filesystem>
#include <iostream>
#include <print>

using namespace catalyst;

bool integration_test() {
    // Setup
    std::println("Starting Integration Test...");
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

    return true;
}
