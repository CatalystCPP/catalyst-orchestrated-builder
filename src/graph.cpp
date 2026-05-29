#include "cbe/graph.hpp"

#include "cbe/build_step.hpp"
#include "cbe/file_handle.hpp"
#include "cbe/utility.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>

namespace fs = std::filesystem;

namespace catalyst {

size_t BuildGraph::get_or_create_node(std::string_view path) {
    if (auto it = index_.find(path); it != index_.end()) {
        return it->second;
    }

    size_t id = nodes_.size();
    nodes_.push_back({path, {}, std::nullopt});
    index_.emplace(path, id);
    return id;
}

namespace {
// Helper to skip whitespace and handle line continuations
const char *skipWhitespace(const char *ptr, const char *end) {
    while (ptr < end) {
        unsigned char c = *ptr;
        if (c > ' ' && c != '\\')
            break;

        if (c <= ' ') {
            ptr++;
        } else if (c == '\\') {
            if (ptr + 1 < end && (ptr[1] == '\n' || ptr[1] == '\r')) {
                ptr++; // skip backslash
                if (ptr < end && *ptr == '\r')
                    ptr++;
                if (ptr < end && *ptr == '\n')
                    ptr++;
            } else {
                break; // Escaped character, start of a filename
            }
        }
    }
    return ptr;
}

// Helper to extract a single token, handling escaped spaces
const char *extractToken(const char *ptr, const char *end, std::string_view &out_token) {
    const char *start = ptr;
    while (ptr < end) {
        unsigned char c = *ptr;
        if (c <= ' ' || c == '\\')
            break;
        ptr++;
    }

    // Handle escaped characters (spaces, etc.)
    if (ptr < end && *ptr == '\\') {
        while (ptr < end) {
            if (*ptr == '\\') {
                if (ptr + 1 >= end) {
                    ptr++; // Dangling backslash
                    break;
                }
                if (ptr[1] == '\n' || ptr[1] == '\r') {
                    break; // Line continuation means end of token
                }
                ptr += 2; // Skip escaped char
            } else if (static_cast<unsigned char>(*ptr) <= ' ') {
                break; // Unescaped whitespace ends token
            } else {
                ptr++;
            }
        }
    }

    out_token = std::string_view(start, ptr - start);
    return ptr;
}
} // namespace

/**
 * @brief Parses a Makefile-style dependency file (.d).
 *
 * This parser handles line continuations (\) and escaped spaces.
 * It invokes the callback for each dependency found.
 *
 * @param graph The build graph (used to keep the memory mapped file alive).
 * @param path The path to the dependency file.
 * @param callback A callable that accepts a std::string_view for each dependency.
 */
void parseDepfile(BuildGraph &graph, const std::filesystem::path &path, auto callback) {
    if (!fs::exists(path)) {
        return;
    }
    auto map = std::make_shared<MappedUnfaultedFile>(path);
    graph.add_resource(map);
    std::string_view content = map->content();

    if (content.empty())
        return;

    const char *ptr = content.data();
    const char *end = ptr + content.size();

    // 1. Skip to deps, ignoring final output
    const char *colon = static_cast<const char *>(std::memchr(ptr, ':', end - ptr));
    if (!colon)
        return;
    ptr = colon + 1;

    // Main parsing loop
    while (ptr < end) {
        ptr = skipWhitespace(ptr, end);
        if (ptr >= end)
            break;

        std::string_view token;
        ptr = extractToken(ptr, end, token);

        if (!token.empty()) {
            callback(token);
        }
    }
}

Result<size_t> BuildGraph::add_step(BuildStep step) {
    size_t out_id = get_or_create_node(step.output);

    if (nodes_[out_id].step_id.has_value()) { // 2 different steps create the same file.
        return std::unexpected(std::format("Duplicate producer for output: {}", step.output));
    }

    // Populate parsed_inputs
    std::string_view remaining = step.inputs;
    // PERF: posibily faster to do with memchr
    while (!remaining.empty()) {
        size_t comma_pos = remaining.find(',');
        std::string_view in_path;
        if (comma_pos == std::string_view::npos) {
            in_path = remaining;
            remaining = {};
        } else {
            in_path = remaining.substr(0, comma_pos);
            remaining = remaining.substr(comma_pos + 1);
        }

        if (in_path.empty())
            continue;

        if (in_path.starts_with('!')) {
            auto opaque_path = in_path.substr(1);
            if (!step.opaque_inputs)
                step.opaque_inputs.emplace();
            step.opaque_inputs->push_back(opaque_path);
        } else {
            step.parsed_inputs.push_back(in_path);
        }
    }

    size_t step_id = steps_.size();
    steps_.push_back(step); // Store the step
    nodes_[out_id].step_id = step_id;

    if (step.tool == "cc" || step.tool == "cxx") {
        auto depfile_parse_callback = [this, out_id, &step = steps_.back()](std::string_view fn) {
            size_t in_id = get_or_create_node(fn);
            this->nodes_[in_id].out_edges.push_back(out_id);
            if (!step.depfile_inputs)
                step.depfile_inputs.emplace();
            step.depfile_inputs->emplace_back(fn);
        };
        const fs::path depfile_path = std::format("{}.d", step.output);
        parseDepfile(*this, depfile_path, depfile_parse_callback);
    } else if (step.tool == "ld" || step.tool == "sld" || step.tool == "ar") {
        // TODO: parse .rsp file
    }

    // Iterate over parsed_inputs to add edges
    for (const auto &in_path : step.parsed_inputs) {
        size_t in_id = get_or_create_node(in_path);
        nodes_[in_id].out_edges.push_back(out_id);
    }

    // Iterate over opaque_inputs to add edges
    if (step.opaque_inputs) {
        for (const auto &in_path : *step.opaque_inputs) {
            size_t in_id = get_or_create_node(in_path);
            nodes_[in_id].out_edges.push_back(out_id);
        }
    }

    return step_id;
}

Result<std::vector<size_t>> BuildGraph::topo_sort() const {
    enum class STATUS : uint8_t { UNSTARTED, WORKING, FINISHED };

    std::vector<STATUS> status(nodes_.size(), STATUS::UNSTARTED);
    std::vector<size_t> order;
    order.reserve(nodes_.size());

    std::function<Result<void>(size_t)> dfs = [&](size_t u) -> Result<void> {
        status[u] = STATUS::WORKING;
        for (size_t v : nodes_[u].out_edges) {
            if (status[v] == STATUS::UNSTARTED) {
                if (auto res = dfs(v); !res)
                    return res;
            } else if (status[v] == STATUS::WORKING) {
                return std::unexpected(std::format("Cycle detected in the build graph at: {}", nodes_[v].path));
            }
        }
        status[u] = STATUS::FINISHED;
        order.push_back(u);
        return {};
    };

    for (size_t i = 0; i < nodes_.size(); ++i) {
        if (status[i] == STATUS::UNSTARTED) {
            if (auto res = dfs(i); !res)
                return std::unexpected(res.error());
        }
    }

    std::ranges::reverse(order);
    return order;
}

} // namespace catalyst
