// Copyright 2026 Siddharth Mohanty
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "cob/build_step.hpp"
#include "cob/flat_map.hpp"
#include "cob/utility.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace catalyst {

/**
 * @brief The out-edges of a graph node.
 *
 * Edges loaded from a binary cache are borrowed from its mapping (which the graph keeps alive) instead of
 * being copied; the list switches to owned storage the first time it is modified.
 */
// NOLINTBEGIN(readability-identifier-naming): mirrors standard container names
class EdgeList {
public:
    using value_type = size_t;
    using const_iterator = const size_t *;
    using iterator = const_iterator;

    EdgeList() = default;

    /** @brief Creates a list that refers to `count` edges at `edges` without copying them. */
    static EdgeList borrow(const size_t *edges, size_t count) noexcept {
        EdgeList list;
        list.borrowed = edges;
        list.borrowed_size = count;
        return list;
    }

    [[nodiscard]] const size_t *data() const noexcept {
        return borrowed ? borrowed : owned.data();
    }
    [[nodiscard]] size_t size() const noexcept {
        return borrowed ? borrowed_size : owned.size();
    }
    [[nodiscard]] bool empty() const noexcept {
        return size() == 0;
    }
    [[nodiscard]] const_iterator begin() const noexcept {
        return data();
    }
    [[nodiscard]] const_iterator end() const noexcept {
        return data() + size();
    }
    size_t operator[](size_t index) const noexcept {
        return data()[index];
    }

    void push_back(size_t edge) {
        if (borrowed) {
            owned.assign(borrowed, borrowed + borrowed_size);
            borrowed = nullptr;
            borrowed_size = 0;
        }
        owned.push_back(edge);
    }

private:
    std::vector<size_t> owned;
    const size_t *borrowed = nullptr;
    size_t borrowed_size = 0;
};
// NOLINTEND(readability-identifier-naming)

/**
 * @brief Represents the dependency graph of the build system.
 *
 * This class manages nodes (files) and edges (dependencies), as well as the list of build steps.
 * It also manages the lifetime of resources (like memory-mapped files) used by the graph strings.
 */
class BuildGraph {
public:
    /** @brief Represents a node in the dependency graph (typically a file). */
    struct Node {
        std::string_view path;
        EdgeList out_edges; ///< Indices of nodes that depend on this node.
        std::optional<size_t> step_id; ///< Index of the BuildStep that produces this node (if any).
    };

    /**
     * @brief Retrieves the index of an existing node or creates a new one.
     * @param path The file path associated with the node.
     * @return The index of the node in the `nodes_` vector.
     */
    size_t getOrCreateNode(std::string_view path);

    /**
     * @brief Adds a new build step to the graph.
     *
     * This method parses the step's inputs, creates necessary nodes and edges,
     * and handles dependency file parsing if applicable.
     *
     * @param step The build step to add.
     * @return The index of the added step, or an error if a producer for the output already exists.
     */
    Result<size_t> addStep(BuildStep step);

    /**
     * @brief Keeps a resource alive for the lifetime of the graph.
     * @param res A shared pointer to the resource (e.g., MappedFile).
     */
    void addResource(std::shared_ptr<void> res) {
        resources.push_back(std::move(res));
    }

    [[nodiscard]] const std::vector<Node> &nodes() const {
        return nodes_m;
    }

    /**
     * @brief Looks up the node for an exact path without creating one.
     * @return The node index, or std::nullopt if no node has this path.
     */
    [[nodiscard]] std::optional<size_t> findNode(std::string_view path) const {
        if (const size_t *ptr = index.find(path)) {
            return *ptr;
        }
        return std::nullopt;
    }
    [[nodiscard]] const std::vector<BuildStep> &steps() const {
        return steps_m;
    }
    std::vector<BuildStep> &steps() {
        return steps_m;
    }

    /**
     * @brief Performs a topological sort of the graph.
     * @return A vector of node indices in topological order (dependencies first),
     *         or an error if a cycle is detected.
     */
    [[nodiscard]] Result<std::vector<size_t>> topoSort() const;

    struct SerializedData {
        std::vector<Node> nodes;
        std::vector<BuildStep> steps;
        FlatHashMap<std::string_view, size_t, StringViewHash> index;
    };

    // NOLINTBEGIN(cppcoreguidelines-rvalue-reference-param-not-moved)
    void loadSerializedData(SerializedData &&data) {
        nodes_m = std::move(data.nodes);
        steps_m = std::move(data.steps);
        index = std::move(data.index);
    }
    // NOLINTEND(cppcoreguidelines-rvalue-reference-param-not-moved)

    friend Result<void> parse(class COBBuilder &, const std::filesystem::path &);

private:
    std::vector<Node> nodes_m;
    std::vector<BuildStep> steps_m;
    FlatHashMap<std::string_view, size_t, StringViewHash> index;
    std::vector<std::shared_ptr<void>> resources;
};

} // namespace catalyst
