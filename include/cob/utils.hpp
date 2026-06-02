#pragma once

#include <cstdint>
#include <string_view>

namespace catalyst {

inline constexpr uint64_t FNV_OFFSET_BASIS = 14695981039346656037ULL;
inline constexpr uint64_t FNV_PRIME = 1099511628211ULL;

uint64_t fnv1a_hash(std::string_view sv, uint64_t hash);

// Helper overload for default hash value
inline uint64_t fnv1a_hash(std::string_view sv) {
    return fnv1a_hash(sv, FNV_OFFSET_BASIS);
}

} // namespace catalyst
