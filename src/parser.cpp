#include "cob/parser.hpp"

#include "cob/binary.hpp"
#include "cob/builder.hpp"
#include "cob/file_handle.hpp"
#include "cob/utility.hpp"

#include <memory>
#include <string_view>

namespace catalyst {

namespace {

Result<void> parseDEF(const std::string_view line, COBBuilder &builder) {
    size_t first_pipe = line.find('|');
    if (first_pipe == std::string_view::npos) {
        return std::unexpected(std::format("Malformed def line (missing first pipe): {}", line));
    }

    size_t second_pipe = line.find('|', first_pipe + 1);
    if (second_pipe == std::string_view::npos) {
        return std::unexpected(std::format("Malformed def line (missing second pipe): {}", line));
    }

    builder.add_definition(line.substr(first_pipe + 1, second_pipe - (first_pipe + 1)), // key
                           line.substr(second_pipe + 1)                                 // value
    );

    return {};
}

Result<void> parseStep(const std::string_view line, COBBuilder &builder) {
    size_t first_pipe = line.find('|');
    if (first_pipe == std::string_view::npos) {
        return std::unexpected(std::format("Malformed step line (missing first pipe): {}", line));
    }

    size_t second_pipe = line.find('|', first_pipe + 1);
    if (second_pipe == std::string_view::npos) {
        return std::unexpected(std::format("Malformed step line (missing second pipe): {}", line));
    }
    Result<void> res = builder.add_step({.tool = line.substr(0, first_pipe),
                                         .inputs = line.substr(first_pipe + 1, second_pipe - (first_pipe + 1)),
                                         .output = line.substr(second_pipe + 1),
                                         .opaque_inputs = {},
                                         .depfile_inputs = {},
                                         .parsed_inputs = {}});
    if (!res) {
        return std::unexpected(res.error());
    }
    return {};
}

} // namespace

Result<void> parse(COBBuilder &builder, const std::filesystem::path &path) {
#if FF_cob__binary == 1
    if (std::filesystem::exists(".catalyst.bin") &&
        std::filesystem::last_write_time(".catalyst.bin") > std::filesystem::last_write_time(path)) {
        return parseBin(builder);
    }
#endif
    std::string_view content;
    try {
        auto file = std::make_shared<MappedFile>(path);
        builder.add_resource(file);
        content = file->content();
    } catch (const std::exception &err) {
        return std::unexpected(err.what());
    }

    size_t start = 0;
    while (start < content.size()) {
        size_t end = content.find('\n', start);
        // last line of the file
        if (end == std::string_view::npos) {
            end = content.size();
        }

        std::string_view line = content.substr(start, end - start);
        // windows CRLF handling
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }

        if (!line.empty()) {
            if (line.starts_with("#")) {
                // Comment
            } else if (line.starts_with("DEF|")) {
                auto res = parseDEF(line, builder);
                if (!res)
                    return res;
            } else [[likely]] {
                auto res = parseStep(line, builder);
                if (!res)
                    return res;
            }
        }

        start = end + 1;
    }
#if FF_cob__binary
    auto _ = emitBin(builder);
#endif
    return {};
}

} // namespace catalyst
