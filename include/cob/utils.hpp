#pragma once

#include "cob/rapidhash.h"
#include <cstdint>
#include <string_view>

namespace catalyst {

inline uint64_t rapid_hash(std::string_view sv, uint64_t seed = 0) {
    return rapidhash_withSeed(sv.data(), sv.size(), seed);
}

} // namespace catalyst
