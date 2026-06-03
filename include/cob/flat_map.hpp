#pragma once

#include "cob/utils.hpp"
#include <vector>
#include <string_view>
#include <cstddef>
#include <cstdint>

namespace catalyst {

constexpr double ROBIN_HOOD_LOAD_FACTOR = 0.75;
constexpr size_t TUNABLE_DEFAULT_MAP_INIITAL_CAPACITY = 16;

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

    explicit FlatHashMap(size_t capacity = TUNABLE_DEFAULT_MAP_INIITAL_CAPACITY);
    ~FlatHashMap() = default;

    FlatHashMap(const FlatHashMap&) = default;
    FlatHashMap& operator=(const FlatHashMap&) = default;
    FlatHashMap(FlatHashMap&&) noexcept = default;
    FlatHashMap& operator=(FlatHashMap&&) noexcept = default;

    Value_T* insert(const Key_T& key, const Value_T& value);

    template <typename... Args_T>
    Value_T* emplace(const Key_T& key, Args_T&&... args) {
        if (size_m >= slots.size() * ROBIN_HOOD_LOAD_FACTOR) {
            rehash(slots.size() * 2);
        }
        return insertHelper(key, Value_T(std::forward<Args_T>(args)...));
    }

    Value_T* find(const Key_T& key);
    const Value_T* find(const Key_T& key) const;

    [[nodiscard]] size_t size() const { return size_m; }
    [[nodiscard]] bool empty() const { return size_m == 0; }
    void reserve(size_t capacity);

private:
    Value_T* insertHelper(Key_T key, Value_T value);
    void rehash(size_t new_capacity);

    std::vector<Slot> slots;
    size_t size_m = 0;
};

} // namespace catalyst
