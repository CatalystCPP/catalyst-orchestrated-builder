#pragma once
#include <expected>
#include <format>
#include <string>

namespace catalyst {
template <typename Value_T> using Result = std::expected<Value_T, std::string>;
} // namespace catalyst
