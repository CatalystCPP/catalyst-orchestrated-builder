#pragma once

#include "cob/builder.hpp"
#include "cob/utility.hpp"

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
Result<void> parseBin(COBBuilder &builder);

/**
 * @brief Serializes the current build graph to the binary cache format.
 *
 * @param builder The builder containing the graph to serialize.
 * @return Success or error.
 */
Result<void> emitBin(COBBuilder &builder);

} // namespace catalyst
