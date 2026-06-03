#pragma once

#include "cob/build_step.hpp"
#include "cob/utility.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include "cob/flat_map.hpp"
#include <vector>

namespace catalyst {

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
        std::vector<size_t> out_edges; ///< Indices of nodes that depend on this node.
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

    //NOLINTBEGIN(cppcoreguidelines-rvalue-reference-param-not-moved)
    void loadSerializedData(SerializedData &&data) {
        nodes_m = std::move(data.nodes);
        steps_m = std::move(data.steps);
        index = std::move(data.index);
    }
    //NOLINTEND(cppcoreguidelines-rvalue-reference-param-not-moved)

    friend Result<void> parse(class COBBuilder &, const std::filesystem::path &);


private:
    std::vector<Node> nodes_m;
    std::vector<BuildStep> steps_m;
    FlatHashMap<std::string_view, size_t, StringViewHash> index;
    std::vector<std::shared_ptr<void>> resources;
};

} // namespace catalyst
