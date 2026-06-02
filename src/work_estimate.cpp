#include "cob/work_estimate.hpp"

#if FF_cbe__estimates

#include "cob/file_handle.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string_view>

catalyst::WorkEstimate::WorkEstimate(const std::filesystem::path &path_to_estimates) {
    try {
        estimates_file_keep_alive = std::make_shared<MappedFile>(path_to_estimates);
        std::string_view content = estimates_file_keep_alive->content();
        size_t start = 0;
        while (start < content.size()) {
            size_t end = content.find('\n', start);
            std::string_view line =
                (end == std::string_view::npos) ? content.substr(start) : content.substr(start, end - start);

#if defined(_WIN32) || defined(_WIN64)
            // windows CRLF handling
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }
#endif
            if (!line.empty()) {
                // format SHOULD ALWAYS be: <file_path>|<estimate_as_int>
                auto pipe_pos = line.find('|');
                if (pipe_pos != std::string_view::npos) {
                    estimates.push_back({line.substr(0, pipe_pos), line.substr(pipe_pos + 1)});
                }
            }

            if (end == std::string_view::npos) {
                break;
            }
            start = end + 1;
        }
        std::ranges::sort(estimates);
    } catch (const std::runtime_error &) {
        std::ignore;
    }
}

[[clang::always_inline]] size_t catalyst::WorkEstimate::getWorkEstimate(std::string_view path) {
    auto it = std::ranges::lower_bound(estimates, path, {}, &EstimateEntry::path);

    if (it != estimates.end() && it->path == path) {
        size_t val = 0;
        auto str = it->value;
        auto [ptr, ec] = std::from_chars(str.data(), str.data() + str.size(), val);
        if (ec == std::errc{}) {
            return val;
        }
    }
    return 0;
}

#endif
