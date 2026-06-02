#pragma once

#include "cob/utility.hpp"

#include <future>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <utility>
namespace catalyst {
/**
 * @brief Executes a subprocess.
 *
 * @param args The command line arguments (first argument is the executable).
 * @param working_dir Optional working directory for the subprocess.
 * @param env Optional environment variables to extend/override the parent environment.
 * @param capture_output If true, captures and returns the combined stdout and stderr of the process.
 * @return A pair containing the exit code of the process (or -1 on error) and the captured output (if requested).
 */
Result<std::pair<int, std::string>> process_exec(std::vector<std::string> &&args,
                         std::optional<std::string> working_dir = std::nullopt,
                         std::optional<std::vector<std::pair<std::string, std::string>>> env = std::nullopt,
                         bool capture_output = false);
} // namespace catalyst
