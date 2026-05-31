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
    size_t idx = get_bucket_index(p);
    Bucket &b = buckets[idx];

    // 1. Shared (read) lock for the fast path (cached hits)
    {
        std::shared_lock<std::shared_mutex> read_lock(b.mtx);
        auto it = std::ranges::lower_bound(b.entries, p, {}, &Entry::path);
        if (it != b.entries.end() && it->path == p) {
            return {it->time, it->ec};
        }
    }

    // 2. Exclusive (write) lock for the fallback path (cache misses)
    std::lock_guard<std::shared_mutex> write_lock(b.mtx);
    auto it = std::ranges::lower_bound(b.entries, p, {}, &Entry::path);
    if (it != b.entries.end() && it->path == p) {
        return {it->time, it->ec};
    }

    std::error_code ec;
    auto time = std::filesystem::last_write_time(p, ec);
    b.entries.insert(it, {.path=p, .time=time, .ec=ec});
    return {time, ec};
}

bool StatCache::changed_since(const std::filesystem::path &input, std::filesystem::file_time_type output_time) {
    auto [input_time, ec] = get_or_update(input);
    if (ec)
        return true;
    return input_time >= output_time;
}

size_t StatCache::get_cache_size() const {
    size_t total = 0;
    for (size_t i = 0; i < NUM_BUCKETS; ++i) {
        std::shared_lock<std::shared_mutex> lock(const_cast<std::shared_mutex&>(buckets[i].mtx));
        total += buckets[i].entries.size();
    }
    return total;
}
