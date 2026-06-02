#include "cob/flat_map.hpp"
#include "cob/binary.hpp"
#include <bit>
#include <utility>

namespace catalyst {


template <typename Key_T, typename Value_T, typename Hash_T>
FlatHashMap<Key_T, Value_T, Hash_T>::FlatHashMap(size_t capacity) {
    slots_.resize(std::bit_ceil(capacity == 0 ? 1UL : capacity));
}

template <typename Key_T, typename Value_T, typename Hash_T>
Value_T* FlatHashMap<Key_T, Value_T, Hash_T>::insert(const Key_T& key, const Value_T& value) {
    if (size_ >= slots_.size() * ROBIN_HOOD_LOAD_FACTOR) {
        rehash(slots_.size() * 2);
    }
    return insert_helper(key, value);
}

template <typename Key_T, typename Value_T, typename Hash_T>
Value_T* FlatHashMap<Key_T, Value_T, Hash_T>::find(const Key_T& key) {
    if (slots_.empty()) return nullptr;

    size_t h = Hash_T{}(key);
    size_t mask = slots_.size() - 1;
    size_t idx = h & mask;
    uint32_t dist = 1;

    while (slots_[idx].probe_distance != 0) {
        if (dist > slots_[idx].probe_distance) {
            return nullptr; // Robin Hood invariant: element is not here
        }
        if (slots_[idx].key == key) {
            return &slots_[idx].value;
        }
        idx = (idx + 1) & mask;
        dist++;
    }
    return nullptr;
}

template <typename Key_T, typename Value_T, typename Hash_T>
const Value_T* FlatHashMap<Key_T, Value_T, Hash_T>::find(const Key_T& key) const {
    if (slots_.empty()) return nullptr;

    size_t h = Hash_T{}(key);
    size_t mask = slots_.size() - 1;
    size_t idx = h & mask;
    uint32_t dist = 1;

    while (slots_[idx].probe_distance != 0) {
        if (dist > slots_[idx].probe_distance) {
            return nullptr; // Robin Hood invariant
        }
        if (slots_[idx].key == key) {
            return &slots_[idx].value;
        }
        idx = (idx + 1) & mask;
        dist++;
    }
    return nullptr;
}

template <typename Key_T, typename Value_T, typename Hash_T>
void FlatHashMap<Key_T, Value_T, Hash_T>::reserve(size_t capacity) {
    size_t needed = std::bit_ceil(static_cast<size_t>(capacity / ROBIN_HOOD_LOAD_FACTOR) + 1);
    if (needed > slots_.size()) {
        rehash(needed);
    }
}

template <typename Key_T, typename Value_T, typename Hash_T>
Value_T* FlatHashMap<Key_T, Value_T, Hash_T>::insert_helper(Key_T key, Value_T value) {
    size_t h = Hash_T{}(key);
    size_t mask = slots_.size() - 1;
    size_t idx = h & mask;

    Key_T current_key = std::move(key);
    Value_T current_value = std::move(value);
    uint32_t current_dist = 1;
    Value_T* inserted_ptr = nullptr;

    while (true) {
        Slot& slot = slots_[idx];

        // Empty slot found
        if (slot.probe_distance == 0) {
            slot.key = std::move(current_key);
            slot.value = std::move(current_value);
            slot.probe_distance = current_dist;
            size_++;
            if (!inserted_ptr) {
                inserted_ptr = &slot.value;
            }
            return inserted_ptr;
        }

        // Existing key found
        if (slot.key == current_key) {
            slot.value = std::move(current_value);
            if (!inserted_ptr) {
                inserted_ptr = &slot.value;
            }
            return inserted_ptr;
        }

        // Robin Hood swap
        if (current_dist > slot.probe_distance) {
            std::swap(current_key, slot.key);
            std::swap(current_value, slot.value);
            std::swap(current_dist, slot.probe_distance);
            if (!inserted_ptr) {
                // Since this slot is swapped, we track its new location
                inserted_ptr = &slot.value;
            }
        }

        idx = (idx + 1) & mask;
        current_dist++;
    }
}

template <typename Key_T, typename Value_T, typename Hash_T>
void FlatHashMap<Key_T, Value_T, Hash_T>::rehash(size_t new_capacity) {
    std::vector<Slot> old_slots = std::move(slots_);
    slots_ = std::vector<Slot>(new_capacity);
    size_ = 0;
    for (auto& slot : old_slots) {
        if (slot.probe_distance != 0) {
            insert_helper(std::move(slot.key), std::move(slot.value));
        }
    }
}

template class FlatHashMap<std::string_view, size_t, StringViewHash>;
template class FlatHashMap<std::string_view, StringRef, StringViewHash>;
} // namespace catalyst
