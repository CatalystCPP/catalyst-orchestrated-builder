#include "cbe/utils.hpp"

namespace catalyst {


uint64_t fnv1a_hash(std::string_view sv, uint64_t hash) {
    for (char c : sv) {
        hash ^= static_cast<uint64_t>(static_cast<unsigned char>(c));
        hash *= FNV_PRIME;
    }
    return hash;
}

} // namespace catalyst
