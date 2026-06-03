#include "cob/binary.hpp"

#include "cob/build_step.hpp"
#include "cob/builder.hpp"
#include "cob/file_handle.hpp"
#include "cob/graph.hpp"
#include "cob/optional_vector.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <memory>
#include <string_view>
#include "cob/flat_map.hpp"
#include <vector>

// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
namespace catalyst {

namespace {

class StringBuffer {
public:
    StringRef add(std::string_view sv) {
        if (StringRef *ptr = buffer_cache.find(sv)) {
            return *ptr;
        }
        uint64_t offset = buffer_data.size();
        uint64_t len = sv.size();
        buffer_data.append(sv);
        StringRef ref = {.offset = offset, .len = len};
        buffer_cache.emplace(sv, ref);
        return ref;
    }

    [[nodiscard]] const std::string &data() const {
        return buffer_data;
    }

private:
    std::string buffer_data;
    FlatHashMap<std::string_view, StringRef, StringViewHash> buffer_cache;
};

constexpr size_t BIN_HEADER_MAGIC_BIT_LEN = 8;

struct BinHeader {
    std::array<char, BIN_HEADER_MAGIC_BIT_LEN> magic;
    uint64_t num_definitions;
    uint64_t num_nodes;
    uint64_t num_steps;
    uint64_t strings_size;
};

struct BinDefinition {
    StringRef key;
    StringRef val;
};

} // namespace

Result<void> parseBin(COBBuilder &builder) {
    std::shared_ptr<MappedFile> file;
    try {
        file = std::make_shared<MappedFile>(".catalyst.bin");
    } catch (const std::exception &e) {
        return std::unexpected(std::format("Failed to mmap .catalyst.bin: {}", e.what()));
    }

    std::string_view content = file->content();
    if (content.size() < sizeof(BinHeader)) {
        return std::unexpected("Malformed .catalyst.bin: too small for header");
    }

    const auto *header = reinterpret_cast<const BinHeader *>(content.data());
#ifdef __linux__
    if (std::memcmp(header->magic.data(), "CATBL002", BIN_HEADER_MAGIC_BIT_LEN) != 0) {
#elif defined(__APPLE__)
    if (std::memcmp(header->magic.data(), "CATBM002", 8) != 0) {
#elif defined(_WIN32) || defined(_WIN64)
    if (std::memcmp(header->magic.data(), "CATBW002", 8) != 0) {
#endif
        return std::unexpected("Invalid magic or version in .catalyst.bin");
    }

    if (header->strings_size > content.size() - sizeof(BinHeader)) {
        return std::unexpected("Malformed .catalyst.bin: strings_size too large");
    }

    const char *ptr = content.data() + sizeof(BinHeader);
    const char *strings_base = content.data() + content.size() - header->strings_size;

    auto get_sv = [&](StringRef ref) -> std::string_view { return {strings_base + ref.offset, ref.len}; };

    // 1. Definitions
    for (uint64_t i = 0; i < header->num_definitions; ++i) {
        const auto *def = reinterpret_cast<const BinDefinition *>(ptr);
        builder.add_definition(get_sv(def->key), get_sv(def->val));
        ptr += sizeof(BinDefinition);
    }

    // 2. Nodes
    std::vector<BuildGraph::Node> nodes;
    FlatHashMap<std::string_view, size_t, StringViewHash> index;
    nodes.reserve(header->num_nodes);
    index.reserve(header->num_nodes);

    for (uint64_t i = 0; i < header->num_nodes; ++i) {
        StringRef path_ref = *reinterpret_cast<const StringRef *>(ptr);
        ptr += sizeof(StringRef);
        uint64_t step_id_raw = *reinterpret_cast<const uint64_t *>(ptr);
        ptr += sizeof(uint64_t);
        uint64_t num_out_edges = *reinterpret_cast<const uint64_t *>(ptr);
        ptr += sizeof(uint64_t);

        std::optional<size_t> step_id = (step_id_raw == UINT64_MAX) ? std::nullopt : std::make_optional(step_id_raw);
        std::vector<size_t> out_edges;
        out_edges.reserve(num_out_edges);
        for (uint64_t j = 0; j < num_out_edges; ++j) {
            out_edges.push_back(*reinterpret_cast<const uint64_t *>(ptr));
            ptr += sizeof(uint64_t);
        }

        std::string_view path = get_sv(path_ref);
        nodes.push_back({.path = path, .out_edges = std::move(out_edges), .step_id = step_id});
        index.emplace(path, i);
    }

    // 3. Steps
    std::vector<BuildStep> steps;
    steps.reserve(header->num_steps);

    for (uint64_t i = 0; i < header->num_steps; ++i) {
        StringRef tool_ref = *reinterpret_cast<const StringRef *>(ptr);
        ptr += sizeof(StringRef);
        StringRef inputs_ref = *reinterpret_cast<const StringRef *>(ptr);
        ptr += sizeof(StringRef);
        StringRef output_ref = *reinterpret_cast<const StringRef *>(ptr);
        ptr += sizeof(StringRef);
        uint64_t command_hash = *reinterpret_cast<const uint64_t *>(ptr);
        ptr += sizeof(uint64_t);
        uint64_t depfile_count = *reinterpret_cast<const uint64_t *>(ptr);
        ptr += sizeof(uint64_t);

        catalyst::optional_vector<std::string_view> depfile_inputs;
        if (depfile_count != UINT64_MAX) {
            depfile_inputs.reserve(depfile_count);
            for (uint64_t j = 0; j < depfile_count; ++j) {
                StringRef ref = *reinterpret_cast<const StringRef *>(ptr);
                ptr += sizeof(StringRef);
                depfile_inputs.push_back(get_sv(ref));
            }
        }

        std::vector<std::string_view> parsed_inputs;
        catalyst::optional_vector<std::string_view> opaque_inputs;
        std::string_view remaining = get_sv(inputs_ref);
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
            if (!in_path.empty()) {
                if (in_path.starts_with('!')) {
                    opaque_inputs.push_back(in_path.substr(1));
                } else {
                    parsed_inputs.push_back(in_path);
                }
            }
        }

        steps.push_back({.tool = get_sv(tool_ref),
                         .inputs = get_sv(inputs_ref),
                         .output = get_sv(output_ref),
                         .opaque_inputs = std::move(opaque_inputs),
                         .depfile_inputs = std::move(depfile_inputs),
                         .parsed_inputs = std::move(parsed_inputs),
                         .command_hash = command_hash});
    }

    builder.load_graph_data(BuildGraph::SerializedData{
        .nodes = std::move(nodes),
        .steps = std::move(steps),
        .index = std::move(index)
    });

    builder.add_resource(file);
    return {};
}

Result<void> emitBin(COBBuilder &builder) {
    std::ofstream out(".catalyst.bin.tmp", std::ios::binary);
    if (!out) {
        return std::unexpected("Failed to open .catalyst.bin.tmp for writing");
    }

    StringBuffer sb;
    const Definitions &definitions = builder.definitions();
    const std::vector<BuildGraph::Node> &nodes = builder.graph().nodes();
    const std::vector<BuildStep> &steps = builder.graph().steps();

    std::vector<BinDefinition> bin_defs;
    for (const auto &[k, v] : definitions) {
        bin_defs.push_back({.key = sb.add(k), .val = sb.add(v)});
    }

    // Nodes and steps are variable length, we'll write them in two passes or buffer.
    // Let's buffer to calculate sizes.
    std::vector<char> nodes_buf;
    for (const BuildGraph::Node &node : nodes) {
        StringRef path_ref = sb.add(node.path);
        nodes_buf.insert(nodes_buf.end(),
                         reinterpret_cast<const char *>(&path_ref),
                         reinterpret_cast<const char *>(&path_ref) + sizeof(StringRef));

        uint64_t step_id = node.step_id.value_or(UINT64_MAX);
        nodes_buf.insert(nodes_buf.end(),
                         reinterpret_cast<const char *>(&step_id),
                         reinterpret_cast<const char *>(&step_id) + sizeof(uint64_t));

        uint64_t num_out_edges = node.out_edges.size();
        nodes_buf.insert(nodes_buf.end(),
                         reinterpret_cast<const char *>(&num_out_edges),
                         reinterpret_cast<const char *>(&num_out_edges) + sizeof(uint64_t));

        for (size_t edge : node.out_edges) {
            uint64_t edge_u64 = edge;
            nodes_buf.insert(nodes_buf.end(),
                             reinterpret_cast<const char *>(&edge_u64),
                             reinterpret_cast<const char *>(&edge_u64) + sizeof(uint64_t));
        }
    }

    std::vector<char> steps_buf;
    for (const BuildStep &step : steps) {
        StringRef tool_ref = sb.add(step.tool);
        StringRef inputs_ref = sb.add(step.inputs);
        StringRef output_ref = sb.add(step.output);

        steps_buf.insert(steps_buf.end(),
                         reinterpret_cast<const char *>(&tool_ref),
                         reinterpret_cast<const char *>(&tool_ref) + sizeof(StringRef));
        steps_buf.insert(steps_buf.end(),
                         reinterpret_cast<const char *>(&inputs_ref),
                         reinterpret_cast<const char *>(&inputs_ref) + sizeof(StringRef));
        steps_buf.insert(steps_buf.end(),
                         reinterpret_cast<const char *>(&output_ref),
                         reinterpret_cast<const char *>(&output_ref) + sizeof(StringRef));

        uint64_t command_hash = step.command_hash;
        steps_buf.insert(steps_buf.end(),
                         reinterpret_cast<const char *>(&command_hash),
                         reinterpret_cast<const char *>(&command_hash) + sizeof(uint64_t));

        uint64_t depfile_count = step.depfile_inputs.has_value() ? step.depfile_inputs.size() : UINT64_MAX;
        steps_buf.insert(steps_buf.end(),
                         reinterpret_cast<const char *>(&depfile_count),
                         reinterpret_cast<const char *>(&depfile_count) + sizeof(uint64_t));

        if (step.depfile_inputs.has_value()) {
            for (const std::string_view &di : step.depfile_inputs) {
                StringRef ref = sb.add(di);
                steps_buf.insert(steps_buf.end(),
                                 reinterpret_cast<const char *>(&ref),
                                 reinterpret_cast<const char *>(&ref) + sizeof(StringRef));
            }
        }
    }

    BinHeader header{};
#ifdef __linux__
    std::memcpy(header.magic.data(), "CATBL002", BIN_HEADER_MAGIC_BIT_LEN);
#elif defined(__APPLE__)
    std::memcpy(header.magic.data(), "CATBM002", 8);
#elif defined(_WIN32) || defined(_WIN64)
    std::memcpy(header.magic.data(), "CATBW002", 8);
#endif
    header.num_definitions = bin_defs.size();
    header.num_nodes = nodes.size();
    header.num_steps = steps.size();
    header.strings_size = sb.data().size();

    // NOLINTBEGIN(cppcoreguidelines-narrowing-conversions, bugprone-narrowing-conversions)
    out.write(reinterpret_cast<const char *>(&header), sizeof(BinHeader));
    out.write(reinterpret_cast<const char *>(bin_defs.data()), bin_defs.size() * sizeof(BinDefinition));
    out.write(nodes_buf.data(), nodes_buf.size());
    out.write(steps_buf.data(), steps_buf.size());
    out.write(sb.data().data(), sb.data().size());
    out.close();
    if (!out) {
        std::error_code rm_ec;
        std::filesystem::remove(".catalyst.bin.tmp", rm_ec);
        return std::unexpected("Failed to write .catalyst.bin.tmp");
    }

    std::error_code ec;
    std::filesystem::rename(".catalyst.bin.tmp", ".catalyst.bin", ec);
    if (ec) {
        return std::unexpected("Failed to rename .catalyst.bin.tmp to .catalyst.bin: " + ec.message());
    }

    return {};
    // NOLINTEND(cppcoreguidelines-narrowing-conversions, bugprone-narrowing-conversions)
}

} // namespace catalyst
// NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
