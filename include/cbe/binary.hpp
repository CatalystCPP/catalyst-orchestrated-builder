#pragma once

#include "cbe/builder.hpp"
#include "cbe/utility.hpp"

namespace catalyst {

struct StringRef {
    uint64_t offset;
    uint64_t len;
};

/**
 * @brief Parses the binary cache format (.catalyst.bin).
 *
 * This is a fast-path loading mechanism that bypasses text parsing.
 *
 * @param builder The builder to populate.
 * @return Success or error.
 */
Result<void> parse_bin(CBEBuilder &builder);

/**
 * @brief Serializes the current build graph to the binary cache format.
 *
 * @param builder The builder containing the graph to serialize.
 * @return Success or error.
 */
Result<void> emit_bin(CBEBuilder &builder);

} // namespace catalyst
