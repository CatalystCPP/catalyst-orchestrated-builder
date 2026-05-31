#pragma once

#include "cbe/utils.hpp"
#include <vector>
#include <string_view>
#include <cstddef>
#include <cstdint>

namespace catalyst {

constexpr double ROBIN_HOOD_LOAD_FACTOR = 0.75;

struct StringViewHash {
    size_t operator()(std::string_view sv) const {
        return static_cast<size_t>(fnv1a_hash(sv));
    }
};

template <typename Key_T, typename Value_T, typename Hash_T = std::hash<Key_T>>
class FlatHashMap {
public:
    struct Slot {
        Key_T key = {};
        Value_T value = {};
        uint32_t probe_distance = 0; // 0 means empty, > 0 is probe distance + 1
    };

    explicit FlatHashMap(size_t capacity = 16);
    ~FlatHashMap() = default;

    FlatHashMap(const FlatHashMap&) = default;
    FlatHashMap& operator=(const FlatHashMap&) = default;
    FlatHashMap(FlatHashMap&&) noexcept = default;
    FlatHashMap& operator=(FlatHashMap&&) noexcept = default;

    Value_T* insert(const Key_T& key, const Value_T& value);

    template <typename... Args_T>
    Value_T* emplace(const Key_T& key, Args_T&&... args) {
        if (size_ >= slots_.size() * ROBIN_HOOD_LOAD_FACTOR) {
            rehash(slots_.size() * 2);
        }
        return insert_helper(key, Value_T(std::forward<Args_T>(args)...));
    }

    Value_T* find(const Key_T& key);
    const Value_T* find(const Key_T& key) const;

    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    void reserve(size_t capacity);

private:
    Value_T* insert_helper(Key_T key, Value_T value);
    void rehash(size_t new_capacity);

    std::vector<Slot> slots_;
    size_t size_ = 0;
};

} // namespace catalyst
