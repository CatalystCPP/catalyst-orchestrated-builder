// Copyright 2026 Siddharth Mohanty
// SPDX-License-Identifier: Apache-2.0

#include "cob/graph.hpp"

#include "cob/build_step.hpp"
#include "cob/file_handle.hpp"
#include "cob/utility.hpp"

#include <algorithm>
#include <bit>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace catalyst {

size_t BuildGraph::getOrCreateNode(std::string_view path) {
    if (size_t *ptr = index.find(path)) {
        return *ptr;
    }

    size_t id = nodes_m.size();
    nodes_m.push_back({.path = path, .out_edges = {}, .step_id = std::nullopt});
    index.emplace(path, id);
    return id;
}

namespace {
// Skip horizontal whitespace and continuations, but never cross a rule boundary.
const char *skipWhitespace(const char *ptr, const char *end) {
    while (ptr < end) {
        unsigned char c = *ptr;
        if (c > ' ' && c != '\\')
            break;

        if (c == '\n' || c == '\r') {
            break;
        }
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

// Finds the first byte that ends a plain token or needs decoding: whitespace and control characters, '\\',
// '#', or '$'. Scans a word at a time: the per-byte tests below can only misfire above a byte that truly
// matches, so on little-endian targets the lowest flagged byte is exact.
const char *findTokenSpecial(const char *ptr, const char *end) {
    if constexpr (std::endian::native == std::endian::little) {
        constexpr uint64_t ONES = 0x0101010101010101ULL;
        constexpr uint64_t HIGHS = 0x8080808080808080ULL;
        constexpr unsigned char FIRST_PLAIN = '!'; // bytes below this end a token
        auto zero_bytes = [](uint64_t word) { return (word - ONES) & ~word & HIGHS; };
        while (end - ptr >= static_cast<std::ptrdiff_t>(sizeof(uint64_t))) {
            uint64_t word = 0;
            std::memcpy(&word, ptr, sizeof(word));
            const uint64_t hits = ((word - (ONES * FIRST_PLAIN)) & ~word & HIGHS) |
                                  zero_bytes(word ^ (ONES * '\\')) | zero_bytes(word ^ (ONES * '#')) |
                                  zero_bytes(word ^ (ONES * '$'));
            if (hits != 0) {
                return ptr + (std::countr_zero(hits) / CHAR_BIT);
            }
            ptr += sizeof(uint64_t);
        }
    }
    while (ptr < end) {
        unsigned char c = *ptr;
        if (c <= ' ' || c == '\\' || c == '#' || c == '$')
            break;
        ptr++;
    }
    return ptr;
}

// Helper to extract a single token, handling escaped spaces. Sets needs_decode if the token contains a
// backslash or a dollar.
const char *extractToken(const char *ptr, const char *end, std::string_view &out_token, bool &needs_decode) {
    const char *start = ptr;
    needs_decode = false;
    ptr = findTokenSpecial(ptr, end);
    while (ptr < end && *ptr == '$') {
        needs_decode = true;
        ptr = findTokenSpecial(ptr + 1, end);
    }

    // Handle escaped characters (spaces, etc.)
    if (ptr < end && *ptr == '\\') {
        needs_decode = true;
        while (ptr < end) {
            if (*ptr == '\\') {
                const char *slash_start = ptr;
                while (ptr < end && *ptr == '\\') {
                    ++ptr;
                }
                if (ptr == end) {
                    break;
                }
                const bool odd_slashes = (ptr - slash_start) % 2 != 0;
                if (*ptr == '\n' || *ptr == '\r') {
                    if (odd_slashes) {
                        --ptr; // Leave the continuation for skipWhitespace.
                    }
                    break;
                }
                // GCC quotes '#' with one extra backslash, unlike whitespace,
                // which doubles preceding backslashes before adding its escape.
                if (*ptr == '#' || (odd_slashes && (*ptr == ' ' || *ptr == '\t'))) {
                    ++ptr;
                }
            } else if (static_cast<unsigned char>(*ptr) <= ' ' || *ptr == '#') {
                break; // Unescaped whitespace ends token
            } else {
                ptr++;
            }
        }
    }

    out_token = std::string_view(start, ptr - start);
    return ptr;
}

// Make quoting is not shell quoting: backslashes before ordinary characters
// are literal, and a dollar is represented by two dollars.
std::string_view decodeDepfileToken(BuildGraph &graph, std::string_view token, bool needs_decode) {
    if (!needs_decode || token.find_first_of("\\$") == std::string_view::npos) {
        return token;
    }

    auto decoded = std::make_shared<std::string>();
    decoded->reserve(token.size());
    for (size_t i = 0; i < token.size();) {
        if (token[i] == '$' && i + 1 < token.size() && token[i + 1] == '$') {
            decoded->push_back('$');
            i += 2;
        } else if (token[i] == '\\') {
            size_t start = i;
            while (i < token.size() && token[i] == '\\') {
                ++i;
            }
            size_t count = i - start;
            if (i < token.size() && token[i] == '#') {
                decoded->append(count - 1, '\\');
                decoded->push_back(token[i++]);
            } else if (i < token.size() && (token[i] == ' ' || token[i] == '\t')) {
                decoded->append(count / 2, '\\');
                decoded->push_back(token[i++]);
            } else {
                decoded->append(count, '\\');
            }
        } else {
            decoded->push_back(token[i++]);
        }
    }
    std::string_view result = *decoded;
    graph.addResource(std::move(decoded));
    return result;
}

/**
 * @brief Parses the first logical rule of a compiler-generated dependency file (.d).
 *
 * Handles LF/CRLF continuations and Make-quoted spaces, tabs, hashes, and dollars.
 * Later rules (including -MP phony targets) are not prerequisites of this output.
 * Ordinary tokens reference the mapping; decoded tokens are owned by the graph.
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
    graph.addResource(map);
    std::string_view content = map->content();

    if (content.empty())
        return;

    const char *ptr = content.data();
    const char *end = ptr + content.size();

    // Find the target separator, skipping escaped colons and drive prefixes.
    const char *target_start = ptr;
    while (ptr < end) {
        if (*ptr == '\n' || *ptr == '\r' || *ptr == '#') {
            return;
        }
        if (*ptr == '\\' && ptr + 1 < end) {
            ++ptr;
            if (*ptr == '\r' && ptr + 1 < end && ptr[1] == '\n') {
                ++ptr;
            }
            ++ptr;
        } else if (*ptr == ':') {
            const bool drive_prefix = ptr == target_start + 1 && ptr + 1 < end && (ptr[1] == '/' || ptr[1] == '\\');
            ++ptr;
            if (!drive_prefix) {
                break;
            }
        } else {
            if (*ptr == ' ' || *ptr == '\t') {
                target_start = ptr + 1;
            }
            ++ptr;
        }
    }

    // Main parsing loop
    while (ptr < end) {
        ptr = skipWhitespace(ptr, end);
        if (ptr >= end || *ptr == '\n' || *ptr == '\r' || *ptr == '#')
            break;

        std::string_view token;
        bool needs_decode = false;
        ptr = extractToken(ptr, end, token, needs_decode);

        if (!token.empty()) {
            callback(decodeDepfileToken(graph, token, needs_decode));
        }
    }
}
} // namespace

Result<size_t> BuildGraph::addStep(BuildStep step) {
    size_t out_id = getOrCreateNode(step.output);

    if (nodes_m[out_id].step_id.has_value()) { // 2 different steps create the same file.
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
            std::string_view opaque_path = in_path.substr(1);
            step.opaque_inputs.push_back(opaque_path);
        } else {
            step.parsed_inputs.push_back(in_path);
        }
    }

    size_t step_id = steps_m.size();
    steps_m.push_back(std::move(step)); // Store the step
    nodes_m[out_id].step_id = step_id;

    BuildStep &live_step = steps_m.back();

    if (live_step.tool == "cc" || live_step.tool == "cxx") {
        auto depfile_parse_callback = [this, out_id, &step = live_step](std::string_view fn) {
            size_t in_id = getOrCreateNode(fn);
            this->nodes_m[in_id].out_edges.push_back(out_id);
            // Same bytes as fn, but the node's view, so equal paths share one address.
            step.depfile_inputs.emplace_back(this->nodes_m[in_id].path);
        };
        const fs::path depfile_path = std::format("{}.d", live_step.output);
        parseDepfile(*this, depfile_path, depfile_parse_callback);
    } else if (live_step.tool == "ld" || live_step.tool == "sld" || live_step.tool == "ar") {
        // TODO: parse .rsp file
    }

    // Iterate over parsed_inputs to add edges
    for (const std::string_view &in_path : live_step.parsed_inputs) {
        size_t in_id = getOrCreateNode(in_path);
        nodes_m[in_id].out_edges.push_back(out_id);
    }

    // Iterate over opaque_inputs to add edges
    if (live_step.opaque_inputs.has_value()) {
        for (const std::string_view &in_path : live_step.opaque_inputs) {
            size_t in_id = getOrCreateNode(in_path);
            nodes_m[in_id].out_edges.push_back(out_id);
        }
    }

    return step_id;
}

Result<std::vector<size_t>> BuildGraph::topoSort() const {
    enum class STATUS : uint8_t { UNSTARTED, WORKING, FINISHED };

    std::vector<STATUS> status(nodes_m.size(), STATUS::UNSTARTED);
    std::vector<size_t> order;
    order.reserve(nodes_m.size());

    struct StackFrame {
        size_t node;
        size_t next_edge_idx;
    };

    std::vector<StackFrame> stack;
    stack.reserve(nodes_m.size());

    for (size_t i = 0; i < nodes_m.size(); ++i) {
        if (status[i] != STATUS::UNSTARTED) {
            continue;
        }

        stack.push_back({.node = i, .next_edge_idx = 0});
        status[i] = STATUS::WORKING;

        while (!stack.empty()) {
            StackFrame &frame = stack.back();
            size_t u = frame.node;
            const BuildGraph::Node &node = nodes_m[u];

            if (frame.next_edge_idx < node.out_edges.size()) {
                size_t v = node.out_edges[frame.next_edge_idx];
                frame.next_edge_idx++; // Advance to next edge for when we return to u

                if (status[v] == STATUS::UNSTARTED) {
                    status[v] = STATUS::WORKING;
                    stack.push_back({.node = v, .next_edge_idx = 0});
                } else if (status[v] == STATUS::WORKING) {
                    return std::unexpected(std::format("Cycle detected in the build graph at: {}", nodes_m[v].path));
                }
            } else {
                // All out edges processed
                status[u] = STATUS::FINISHED;
                order.push_back(u);
                stack.pop_back();
            }
        }
    }

    std::ranges::reverse(order);
    return order;
}

} // namespace catalyst
