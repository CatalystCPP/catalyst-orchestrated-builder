#include "cob/executor.hpp"
#include "tests/test_suite.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

using namespace catalyst;

bool stat_cache_test() {
    std::cout << "Running StatCache Thread-Safety Test..." << std::endl;

    // Create a dummy file
    std::filesystem::path dummy_file = "test_stat_cache_dummy.txt";
    {
        std::ofstream f(dummy_file);
        f << "dummy content";
    }

    StatCache cache;

    // We will spawn multiple threads that repeatedly query the same path
    constexpr int num_threads = 16;
    constexpr int iterations = 1000;
    std::vector<std::thread> threads;

    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&cache, &dummy_file]() {
            for (int j = 0; j < iterations; ++j) {
                auto [time, ec] = cache.get_or_update(dummy_file);
                assert(!ec);
            }
        });
    }

    for (auto &t : threads) {
        t.join();
    }

    // Now inspect the cache size
    std::cout << "Cache size after concurrent runs: " << cache.get_cache_size() << std::endl;

    // The size of the cache MUST be exactly 1, because we only queried one unique file!
    // If there is a check-then-act race, duplicates would have been inserted, making size > 1.
    assert(cache.get_cache_size() == 1);

    // Clean up
    std::filesystem::remove(dummy_file);

    std::cout << "StatCache Thread-Safety Test Passed successfully!" << std::endl;
    return true;
}
