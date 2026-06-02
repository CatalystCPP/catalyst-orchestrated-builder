#pragma once

#if FF_cob__estimates

#include "cob/file_handle.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

namespace catalyst {
class WorkEstimate {
public:
    explicit WorkEstimate(const std::filesystem::path &path_to_estimates);
    explicit WorkEstimate(std::filesystem::path &&path_to_estimates) : WorkEstimate(path_to_estimates) {
    }

    [[clang::always_inline]] size_t getWorkEstimate(std::string_view path);

private:
    struct EstimateEntry {
        std::string_view path;
        std::string_view value;

        auto operator<=>(const EstimateEntry &other) const = default;
    };

    std::shared_ptr<MappedFile> estimates_file_keep_alive;
    std::vector<EstimateEntry> estimates;
};
}; // namespace catalyst

#endif
