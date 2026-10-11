// Copyright 2026 Siddharth Mohanty
// SPDX-License-Identifier: Apache-2.0

// clang-format off
/*
 * StringRef:
 *      offset  uint64_t Offset into the string pool at the end of the file.
 *      len     uint64_t Length of the string in bytes.
 *
 * Structure of the binary:
 *
 * BinHeader     <-- fixed size
 * Definitions
 * Nodes
 * BuildSteps
 * String Data   <-- Pool of strings that are referenced by the above structures using StringRef (offset + length).
 *                   Strings are not necessarily null-terminated, so the length is required to read them.
 *                   Strings are deduplicated, so the same string may be referenced multiple times.
 *
 * BinHeader:
 *      char[8] Magic   CATB + (L for Linux, M for Mac, W for Windows) + Version Number (3 bytes).
 *      EndianMarker    uint64_t Native-endian representation of 0x0102030405060708.
 *      NumDefinitions  uint64_t
 *      NumNodes        uint64_t
 *      NumSteps        uint64_t
 *      StringsSize     uint64_t Size of the string pool at the end of the file.
 *      Checksum        uint64_t Seeded hash of the header with this field zeroed and the serialized payload.
 *
 * Contiguous block of definitions:
 * Definition:     (total 32 bytes)
 *      StringRef Key
 *      StringRef Value
 *
 * Contiguous block of nodes:
 * Node:           (variable size)
 *      Path StringRef
 *      StepID uint64_t (UINT64_MAX if no step is associated with this node)
 *      EdgeCount uint64_t
 *      Edges uint64_t[EdgeCount] indicing the output edges of this node using an adjacency list.
 *
 * BinStepHeader:  (total 72 bytes, including trailing alignment padding)
 *      Inputs StringRef (comma-separated list of input paths, with opaque inputs prefixed with '!')
 *      Output StringRef (path to the output file)
 *      ExtraFlags StringRef (additional flags to pass to the tool, e.g. for cxx: -std=c++20 -Wall -Werror)
 *      CommandHash uint64_t (hash of the command line, used to determine if the command has changed since the last build)
 *      DepfileCount (number of depfile inputs, UINT64_MAX if no depfile is associated with this step)
 *      Tool ToolKind uint8_t (cc, cxx, ld, ar, or sld)
 *
 * DepfileInputs:
 *      StringRef[DepfileCount] (paths to the depfile inputs, if any)
 */
// clang-format on

#include "cob/binary.hpp"

#include "cob/build_step.hpp"
#include "cob/builder.hpp"
#include "cob/file_handle.hpp"
#include "cob/flat_map.hpp"
#include "cob/graph.hpp"
#include "cob/optional_vector.hpp"
#include "cob/utility.hpp"
#ifdef __linux__
#include "cob/file_descriptor.hpp"
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <cerrno>
#include <cstdint>
#include <cstring>
#if !defined(__linux__)
#include <fstream>
#endif
#include <limits>
#include <memory>
#include <string_view>
#include <tuple>
#include <vector>

#ifdef __linux__
#include <fcntl.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
namespace catalyst {

namespace {

constexpr std::string_view MAGIC = "CATB"
#ifdef __linux__
                                   "L"
#elif defined(__APPLE__)
                                   "M"
#elif defined(_WIN32) || defined(_WIN64)
                                   "W"
#endif
                                   // Revision 005 invalidates graphs with undecoded depfile paths
                                   // or phony targets incorrectly stored as prerequisites.
                                   "005";

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

    void reserve(size_t strings) {
        buffer_cache.reserve(strings);
    }

private:
    std::string buffer_data;
    FlatHashMap<std::string_view, StringRef, StringViewHash> buffer_cache;
};

/**
 * @brief Pool references keyed by string address rather than content.
 *
 * Depfile inputs view the same bytes as their node's path (both in graphs loaded from a cache and in graphs
 * built from depfiles), so most lookups resolve by address without hashing or comparing contents.
 */
class AddressCache {
public:
    explicit AddressCache(size_t capacity) : slots(std::bit_ceil((capacity * 2) + 2)) {
        shift = std::numeric_limits<uint64_t>::digits - std::countr_zero(slots.size());
    }

    /** @brief Remembers the reference of a string that was added to the pool. Ignored when full. */
    void insert(std::string_view sv, StringRef ref) {
        if (sv.data() == nullptr || used + 1 >= slots.size() / 2) {
            return;
        }
        for (size_t idx = index(sv.data());; idx = (idx + 1) & (slots.size() - 1)) {
            Slot &slot = slots[idx];
            if (slot.data == nullptr) {
                slot = {.data = sv.data(), .size = sv.size(), .ref = ref};
                ++used;
                return;
            }
            if (slot.data == sv.data() && slot.size == sv.size()) {
                return;
            }
        }
    }

    [[nodiscard]] const StringRef *find(std::string_view sv) const {
        if (sv.data() == nullptr) {
            return nullptr;
        }
        for (size_t idx = index(sv.data());; idx = (idx + 1) & (slots.size() - 1)) {
            const Slot &slot = slots[idx];
            if (slot.data == nullptr) {
                return nullptr;
            }
            if (slot.data == sv.data() && slot.size == sv.size()) {
                return &slot.ref;
            }
        }
    }

private:
    struct Slot {
        const char *data = nullptr;
        size_t size = 0;
        StringRef ref{};
    };

    [[nodiscard]] size_t index(const char *data) const {
        constexpr uint64_t FIBONACCI_MULTIPLIER = 0x9E3779B97F4A7C15ULL;
        return static_cast<size_t>((reinterpret_cast<uintptr_t>(data) * FIBONACCI_MULTIPLIER) >> shift);
    }

    std::vector<Slot> slots;
    size_t used = 0;
    int shift = 0;
};

struct FreeDeleter {
    void operator()(char *ptr) const noexcept {
        std::free(ptr); // NOLINT(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory)
    }
};

struct BinHeader {
    enum class EndianMarker : uint64_t {
        EXPECTED = 0x0102030405060708ULL,
        BYTE_REVERSED = 0x0807060504030201ULL,
    };

    std::array<char, MAGIC.size()> magic;
    EndianMarker endian_marker;
    uint64_t num_definitions;
    uint64_t num_nodes;
    uint64_t num_steps;
    uint64_t strings_size;
    uint64_t checksum;
};

struct BinDefinition {
    StringRef key;
    StringRef val;
};

enum class ToolKind : std::uint8_t {
    CC_COMPILE = 0,
    CXX_COMPILE = 1,
    LINK = 2,
    ARCHIVE = 3,
    SHARED_LINK = 4,

    INVALID = std::numeric_limits<std::uint8_t>::max(),
};

struct BinStepHeader {
    StringRef inputs;
    StringRef output;
    StringRef extra_flags;
    uint64_t command_hash;
    uint64_t depfile_count;
    ToolKind tool;
    std::array<char, 7> padding; // Padding to align the struct to 8 bytes
};

constexpr ToolKind toolKindFromString(std::string_view tool) {
    if (tool == "cc") {
        return ToolKind::CC_COMPILE;
    }
    if (tool == "cxx") {
        return ToolKind::CXX_COMPILE;
    }
    if (tool == "ld") {
        return ToolKind::LINK;
    }
    if (tool == "ar") {
        return ToolKind::ARCHIVE;
    }
    if (tool == "sld") {
        return ToolKind::SHARED_LINK;
    }
    return ToolKind::INVALID;
}

constexpr std::string_view stringFromToolKind(ToolKind tool) {
    switch (tool) {
        case ToolKind::CC_COMPILE:
            return "cc";
        case ToolKind::CXX_COMPILE:
            return "cxx";
        case ToolKind::LINK:
            return "ld";
        case ToolKind::ARCHIVE:
            return "ar";
        case ToolKind::SHARED_LINK:
            return "sld";
        case ToolKind::INVALID:
        default:
            return {};
    }
}

uint64_t calculateChecksum(const BinHeader &header, std::string_view payload) {
    BinHeader checksum_header = header;
    checksum_header.checksum = 0;
    std::string_view header_bytes{reinterpret_cast<const char *>(&checksum_header), sizeof(BinHeader)};
    return rapid_hash(payload, rapid_hash(header_bytes));
}

Result<void> writeBinData(const BinHeader &header, std::string_view payload) {
#ifdef __linux__
    constexpr mode_t FILE_MODE = 0666;
    FileDescriptor file(::open(".catalyst.bin.tmp", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, FILE_MODE));
    if (file.get() == -1) {
        return std::unexpected("Failed to open .catalyst.bin.tmp for writing");
    }

    constexpr size_t BUFFER_COUNT = 2;
    // POSIX specifies mutable iovec pointers even though writev does not modify the buffers.
    // NOLINTBEGIN(cppcoreguidelines-pro-type-const-cast)
    std::array<iovec, BUFFER_COUNT> buffers{{
        {.iov_base = const_cast<BinHeader *>(&header), .iov_len = sizeof(BinHeader)},
        {.iov_base = const_cast<char *>(payload.data()), .iov_len = payload.size()},
    }};
    // NOLINTEND(cppcoreguidelines-pro-type-const-cast)

    iovec *next_buffer = buffers.data();
    const iovec *buffers_end = buffers.data() + buffers.size();
    while (next_buffer != buffers_end) {
        int remaining_buffers = static_cast<int>(buffers_end - next_buffer);
        auto written = ::writev(file.get(), next_buffer, remaining_buffers);
        if (written == -1 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return std::unexpected("Failed to write .catalyst.bin.tmp");
        }

        auto consumed = static_cast<size_t>(written);
        while (next_buffer != buffers_end && consumed >= next_buffer->iov_len) {
            consumed -= next_buffer->iov_len;
            ++next_buffer;
        }
        if (next_buffer != buffers_end && consumed != 0) {
            auto *data = static_cast<char *>(next_buffer->iov_base);
            next_buffer->iov_base = data + consumed;
            next_buffer->iov_len -= consumed;
        }
    }

    if (!file.close()) {
        return std::unexpected("Failed to close .catalyst.bin.tmp after writing");
    }
#else
    std::ofstream out(".catalyst.bin.tmp", std::ios::binary);
    if (!out) {
        return std::unexpected("Failed to open .catalyst.bin.tmp for writing");
    }

    out.write(reinterpret_cast<const char *>(&header), sizeof(BinHeader));
    out.write(payload.data(), payload.size());
    out.close();
    if (!out) {
        return std::unexpected("Failed to write .catalyst.bin.tmp");
    }
#endif

    return {};
}

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
    const char *content_body_ptr = content.data() + sizeof(BinHeader);
    const char *strings_base = nullptr;
    uint64_t num_definitions = 0;
    std::vector<BuildGraph::Node> nodes;
    FlatHashMap<std::string_view, size_t, StringViewHash> index;
    std::vector<BuildStep> steps;
    auto get_sv = [&](StringRef ref) -> std::string_view { return {strings_base + ref.offset, ref.len}; };

    auto parse_header = [&content, &header, &strings_base, &num_definitions]() -> Result<void> {
        if (std::memcmp(header->magic.data(), MAGIC.data(), MAGIC.size()) != 0) {
            return std::unexpected("Invalid magic or version in .catalyst.bin");
        }

        if (header->endian_marker != BinHeader::EndianMarker::EXPECTED) {
            if (header->endian_marker == BinHeader::EndianMarker::BYTE_REVERSED) {
                return std::unexpected("Incompatible endianness in .catalyst.bin");
            }
            return std::unexpected("Invalid endianness marker in .catalyst.bin");
        }

        std::string_view payload = content.substr(sizeof(BinHeader));
        if (calculateChecksum(*header, payload) != header->checksum) {
            return std::unexpected("Checksum mismatch in .catalyst.bin");
        }

        if (header->strings_size > content.size() - sizeof(BinHeader)) {
            return std::unexpected("Malformed .catalyst.bin: strings_size too large");
        }
        strings_base = content.data() + content.size() - header->strings_size;
        num_definitions = header->num_definitions;
        return {};
    };

    auto parse_definitions = [&content_body_ptr, &num_definitions, &builder, get_sv]() -> void {
        const auto *defs = reinterpret_cast<const BinDefinition *>(content_body_ptr);
        std::for_each_n(defs, num_definitions, [&builder, get_sv](const BinDefinition &def) -> void {
            builder.addDefinition(get_sv(def.key), get_sv(def.val));
        });
        content_body_ptr += num_definitions * sizeof(BinDefinition);
    };

    auto parse_nodes = [&content_body_ptr, &header, &nodes, &index, get_sv]() -> void {
        nodes.reserve(header->num_nodes);
        index.reserve(header->num_nodes);

        for (uint64_t i = 0; i < header->num_nodes; ++i) {
            StringRef path_ref = *reinterpret_cast<const StringRef *>(content_body_ptr);
            content_body_ptr += sizeof(StringRef);
            uint64_t step_id_raw = *reinterpret_cast<const uint64_t *>(content_body_ptr);
            content_body_ptr += sizeof(uint64_t);
            uint64_t num_out_edges = *reinterpret_cast<const uint64_t *>(content_body_ptr);
            content_body_ptr += sizeof(uint64_t);

            std::optional<size_t> step_id =
                (step_id_raw == UINT64_MAX) ? std::nullopt : std::make_optional(step_id_raw);
            // Edges are borrowed from the mapping, which the graph keeps alive.
            static_assert(sizeof(size_t) == sizeof(uint64_t));
            EdgeList out_edges = EdgeList::borrow(reinterpret_cast<const size_t *>(content_body_ptr), num_out_edges);
            content_body_ptr += num_out_edges * sizeof(uint64_t);

            std::string_view path = get_sv(path_ref);
            nodes.push_back({.path = path, .out_edges = std::move(out_edges), .step_id = step_id});
            index.emplace(path, i);
        }
    };

    auto parse_steps = [&content_body_ptr, &header, &steps, &strings_base, get_sv]() -> Result<void> {
        steps.reserve(header->num_steps);

        for (uint64_t i = 0; i < header->num_steps; ++i) {
            const auto *step_header = reinterpret_cast<const BinStepHeader *>(content_body_ptr);
            content_body_ptr += sizeof(BinStepHeader);

            std::string_view tool = stringFromToolKind(step_header->tool);
            if (tool.empty()) {
                return std::unexpected(std::format("Malformed .catalyst.bin: invalid tool kind {}",
                                                   static_cast<unsigned int>(step_header->tool)));
            }

            // Depfile inputs are borrowed from the mapping and decoded on access.
            DepfileInputs depfile_inputs;
            if (step_header->depfile_count != UINT64_MAX) {
                depfile_inputs = DepfileInputs::borrow(
                    reinterpret_cast<const StringRef *>(content_body_ptr), step_header->depfile_count, strings_base);
                content_body_ptr += step_header->depfile_count * sizeof(StringRef);
            }

            std::vector<std::string_view> parsed_inputs;
            catalyst::optional_vector<std::string_view> opaque_inputs;
            std::string_view remaining = get_sv(step_header->inputs);
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

            steps.push_back({.tool = tool,
                             .inputs = get_sv(step_header->inputs),
                             .output = get_sv(step_header->output),
                             .opaque_inputs = std::move(opaque_inputs),
                             .depfile_inputs = std::move(depfile_inputs),
                             .parsed_inputs = std::move(parsed_inputs),
                             .extra_flags = get_sv(step_header->extra_flags),
                             .command_hash = step_header->command_hash});
        }
        return {};
    };

    Result<void> result;
    if (result = parse_header(); !result)
        return result;
    parse_definitions();
    parse_nodes();
    if (result = parse_steps(); !result)
        return result;

    builder.loadGraphData(
        BuildGraph::SerializedData{.nodes = std::move(nodes), .steps = std::move(steps), .index = std::move(index)});

    builder.addResource(file);
    return {};
}

Result<void> emitBin(COBBuilder &builder) {
    const Definitions &definitions = builder.definitions();
    const std::vector<BuildGraph::Node> &nodes = builder.graph().nodes();
    const std::vector<BuildStep> &steps = builder.graph().steps();

    // Size the fixed-layout records up front so each byte is written once, with no reallocation.
    size_t records_size = definitions.size() * sizeof(BinDefinition);
    for (const BuildGraph::Node &node : nodes) {
        records_size += sizeof(StringRef) + (2 * sizeof(uint64_t)) + (node.out_edges.size() * sizeof(uint64_t));
    }
    for (const BuildStep &step : steps) {
        if (toolKindFromString(step.tool) == ToolKind::INVALID) {
            return std::unexpected(std::format("Cannot emit .catalyst.bin: unsupported tool '{}'", step.tool));
        }
        records_size += sizeof(BinStepHeader) + (step.depfile_inputs.size() * sizeof(StringRef));
    }

    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory)
    std::unique_ptr<char, FreeDeleter> payload(static_cast<char *>(std::malloc(records_size + 1)));
    if (!payload) {
        return std::unexpected("Cannot emit .catalyst.bin: out of memory");
    }
    char *out = payload.get();
    auto put = [&out](const auto &value) {
        std::memcpy(out, &value, sizeof(value));
        out += sizeof(value);
    };

    StringBuffer sb;
    sb.reserve(nodes.size() + (2 * steps.size()) + (2 * definitions.size()));
    AddressCache node_paths(nodes.size());

    for (const auto &[k, v] : definitions) {
        put(BinDefinition{.key = sb.add(k), .val = sb.add(v)});
    }

    static_assert(sizeof(size_t) == sizeof(uint64_t));
    for (const BuildGraph::Node &node : nodes) {
        StringRef path_ref = sb.add(node.path);
        node_paths.insert(node.path, path_ref);
        put(path_ref);
        put(static_cast<uint64_t>(node.step_id.value_or(UINT64_MAX)));
        put(static_cast<uint64_t>(node.out_edges.size()));
        if (!node.out_edges.empty()) {
            const size_t edge_bytes = node.out_edges.size() * sizeof(uint64_t);
            std::memcpy(out, node.out_edges.data(), edge_bytes);
            out += edge_bytes;
        }
    }

    for (const BuildStep &step : steps) {
        BinStepHeader step_header{};
        step_header.inputs = sb.add(step.inputs);
        step_header.output = sb.add(step.output);
        step_header.extra_flags = sb.add(step.extra_flags);
        step_header.command_hash = step.command_hash;
        step_header.depfile_count = step.depfile_inputs.has_value() ? step.depfile_inputs.size() : UINT64_MAX;
        step_header.tool = toolKindFromString(step.tool);
        std::memset(step_header.padding.data(), 0, step_header.padding.size());
        put(step_header);

        for (std::string_view input : step.depfile_inputs) {
            const StringRef *known = node_paths.find(input);
            put(known ? *known : sb.add(input));
        }
    }

    BinHeader header{};
    std::memcpy(header.magic.data(), MAGIC.data(), MAGIC.size());
    header.endian_marker = BinHeader::EndianMarker::EXPECTED;
    header.num_definitions = definitions.size();
    header.num_nodes = nodes.size();
    header.num_steps = steps.size();
    header.strings_size = sb.data().size();

    // The string pool is small after deduplication; append it after the records.
    const size_t payload_size = records_size + sb.data().size();
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory)
    char *grown = static_cast<char *>(std::realloc(payload.get(), payload_size + 1));
    if (!grown) {
        return std::unexpected("Cannot emit .catalyst.bin: out of memory");
    }
    std::ignore = payload.release();
    payload.reset(grown);
    std::memcpy(payload.get() + records_size, sb.data().data(), sb.data().size());
    header.checksum = calculateChecksum(header, {payload.get(), payload_size});

    // NOLINTBEGIN(cppcoreguidelines-narrowing-conversions, bugprone-narrowing-conversions)
    if (auto write_result = writeBinData(header, {payload.get(), payload_size}); !write_result) {
        std::error_code rm_ec;
        std::filesystem::remove(".catalyst.bin.tmp", rm_ec);
        return write_result;
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
