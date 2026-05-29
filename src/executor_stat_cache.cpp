#include "cbe/executor.hpp"

#include <algorithm>
#include <filesystem>
using catalyst::StatCache;

bool StatCache::Entry::operator<(const Entry &other) const {
    return path < other.path;
}
bool StatCache::Entry::operator<(const std::filesystem::path &other_path) const {
    return path < other_path;
}

auto StatCache::get_or_update(const std::filesystem::path &p)
    -> std::pair<std::filesystem::file_time_type, std::error_code> {
    {
        std::shared_lock read_lock(cache_mtx);

        auto it = std::ranges::lower_bound(cache, p, {}, &Entry::path);

        if (it != cache.end() && it->path == p) {
            return {it->time, it->ec};
        }
    }

    std::lock_guard write_lock(cache_mtx);
    auto it = std::ranges::lower_bound(cache, p, {}, &Entry::path);

    // Double-check lock pattern (check-then-act race prevention):
    // The shared read lock was released before acquiring this unique write lock.
    // If another thread populated the cache for this path in that brief window,
    // we return the existing entry to prevent duplicates and redundant filesystem queries.
    if (it != cache.end() && it->path == p) {
        return {it->time, it->ec};
    }

    std::error_code ec;
    auto time = std::filesystem::last_write_time(p, ec);

    cache.insert(it, {.path=p, .time=time, .ec=ec}); // it is the element right below so we can add here safely
    return {time, ec};
}

bool StatCache::changed_since(const std::filesystem::path &input, std::filesystem::file_time_type output_time) {
    auto [input_time, ec] = get_or_update(input);
    if (ec)
        return true; // If input missing/error, assume rebuild needed
    return input_time >= output_time;
}

size_t StatCache::get_cache_size() const {
    std::shared_lock read_lock(const_cast<std::shared_mutex&>(cache_mtx));
    return cache.size();
}
