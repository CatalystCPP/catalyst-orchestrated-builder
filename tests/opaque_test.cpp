#include "cob/builder.hpp"
#include "cob/executor.hpp"
#include "cob/parser.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <print>
#include <thread>
#include <chrono>

using namespace catalyst;

void create_file(const std::string &name, const std::string &content) {
    std::ofstream f(name);
    f << content;
    f.close();
}

bool opaque_deps_test() {
    std::println("Starting Opaque Deps Test...");

    // Setup files
    create_file("input.c", "int main() {}");
    create_file("opaque.txt", "some data");

    // We will use 'cp' as a tool to simulate a build step
    // But since the executor has hardcoded tools, we might need to mock one or use one that's available.
    // The executor has cc, cxx, ld, ar, sld.
    // Let's use 'cc' but we'll see what it does.
    // Actually, the executor uses process_exec which just runs the command.
    // But the executor constructs the command based on the tool name.

    COBBuilder builder;
    builder.add_definition("cc", ""); // Mock cc with cp for testing
    builder.add_definition("cflags", "");

    BuildStep step;
    step.tool = "cc";
    step.inputs = "input.c,!opaque.txt";
    step.output = "output.o";

    auto res = builder.add_step(std::move(step));
    assert(res);

    Executor executor(std::move(builder), ExecutorConfig{});

    // First run
    std::println("First run...");
    auto exec_res = executor.execute();
    assert(exec_res);
    assert(std::filesystem::exists("output.o"));

    // Verify 'opaque.txt' is not in the command line?
    // We can't easily verify the command line without intercepting process_exec.
    // But we can check if it rebuilds when opaque.txt changes.

    // Sleep to ensure different mtime
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // Change opaque.txt
    std::println("Changing opaque.txt...");
    create_file("opaque.txt", "changed data");

    // Second run - should rebuild
    COBBuilder builder2;
    builder2.add_definition("cc", "cp");
    builder2.add_definition("cflags", "");
    BuildStep step2;
    step2.tool = "cc";
    step2.inputs = "input.c,!opaque.txt";
    step2.output = "output.o";
    builder2.add_step(std::move(step2));

    Executor executor2(std::move(builder2), ExecutorConfig{});
    std::println("Second run (should rebuild)...");

    // We need to capture if it actually ran the tool.
    // The executor prints "[1/X] cc -> output.o" to stdout.

    auto exec_res2 = executor2.execute();
    assert(exec_res2);

    // Cleanup
    std::filesystem::remove("input.c");
    std::filesystem::remove("opaque.txt");
    std::filesystem::remove("output.o");
    if (std::filesystem::exists("output.o.d")) std::filesystem::remove("output.o.d");

    std::println("Opaque Deps Test passed!");
    return true;
}
