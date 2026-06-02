#pragma once

#include "cob/builder.hpp"
#include "cob/utility.hpp"

#include <filesystem>

namespace catalyst {

/**
 * @brief Parses a build manifest file.
 *
 * Detects if a binary cache exists and is up-to-date; if so, loads that.
 * Otherwise, parses the text-based manifest format.
 *
 * @param builder The builder to populate with parsed steps and definitions.
 * @param path The path to the manifest file (typically "catalyst.build").
 * @return Success or error.
 */
Result<void> parse(COBBuilder &builder, const std::filesystem::path &path);

} // namespace catalyst
