#pragma once

#include "cob/graph.hpp"

#include <filesystem>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace catalyst {

/**
 * @brief Builder class for constructing the build graph.
 *
 * This class acts as a facade for populating the `BuildGraph` and `Definitions`.
 * It is used by parsers to construct the build representation.
 */
class COBBuilder {
public:
    /**
     * @brief Adds a build step to the underlying graph.
     * @param bs The build step to add.
     * @return Success or error.
     */
    Result<void> add_step(BuildStep &&bs) {
        auto res = graph_.addStep(std::move(bs));
        if (!res)
            return std::unexpected(res.error());
        return {};
    }

    const BuildGraph &graph() const {
        return graph_;
    }

    /**
     * @brief Returns the built graph by moving it out of the builder.
     * @return The completed `BuildGraph`.
     */
    BuildGraph &&emit_graph() {
        return std::move(graph_);
    }

    /**
     * @brief Adds a global definition/variable.
     * @param key The name of the definition.
     * @param value The value of the definition.
     */
    void add_definition(std::string_view key, std::string_view value) {
        definitions_.emplace(key, value);
    }

    /**
     * @brief Registers a resource to be managed by the graph's lifetime.
     * @param res A shared pointer to the resource.
     */
    void add_resource(std::shared_ptr<void> res) {
        graph_.addResource(std::move(res));
    }

    const Definitions &definitions() const {
        return definitions_;
    }

    void load_graph_data(BuildGraph::SerializedData &&data) {
        graph_.loadSerializedData(std::move(data));
    }

    friend Result<void> parse(COBBuilder &, const std::filesystem::path &);
    friend class Executor;

private:
    template <typename Return_T> Return_T getDefinitionOf(std::string_view key) const {
        if constexpr (std::is_same_v<Return_T, std::string>) {
            if (const Definitions::const_iterator it = definitions_.find(key); it != definitions_.end())
                return std::string(it->second);
            return "";
        } else if constexpr (std::is_same_v<Return_T, std::vector<std::string>>) {
            auto def = getDefinitionOf<std::string>(key);
            return std::ranges::views::split(def, ' ') | std::ranges::to<std::vector<std::string>>();
        } else {
            static_assert(std::false_type(), "Unsupported Return_T for getDefinitionOf");
        }
    }

    std::vector<std::string> getLinkerVec(const std::vector<std::string> &cxx_vec) const {
        auto linker_vec = getDefinitionOf<std::vector<std::string>>("linker");
        if (linker_vec.empty() || (linker_vec.size() == 1 && linker_vec[0].empty())) {
            linker_vec = cxx_vec;
        }
        return linker_vec;
    }

    std::vector<std::string> getArchiverVec() const {
        auto archiver_vec = getDefinitionOf<std::vector<std::string>>("archiver");
        if (archiver_vec.empty() || (archiver_vec.size() == 1 && archiver_vec[0].empty())) {
#if defined(__APPLE__)
            archiver_vec = std::vector<std::string>{"libtool", "-static", "-no_warning_for_no_symbols", "-o"};
#elif defined(_WIN32)
            archiver_vec = std::vector<std::string>{"lib", "/nologo", "/out:"};
#else
            archiver_vec = std::vector<std::string>{"ar"};
#endif
        }
        return archiver_vec;
    }

    BuildGraph graph_;
    Definitions definitions_;
};

} // namespace catalyst
