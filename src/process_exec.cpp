#include "cbe/process_exec.hpp"

#include "cbe/utility.hpp"

#include <expected>
#include <future>
#include <optional>
#include <reproc++/run.hpp>
#include <string>
#include <utility>
#include <vector>

namespace catalyst {
Result<std::pair<int, std::string>> process_exec(std::vector<std::string> &&args,
                         std::optional<std::string> working_dir,
                         std::optional<std::vector<std::pair<std::string, std::string>>> env,
                         bool capture_output) {
    if (args.empty()) {
        return std::unexpected("Cannot execute empty command");
    }

    reproc::options options;
    std::string captured;
    if (capture_output) {
        options.redirect.out.type = reproc::redirect::pipe;
        options.redirect.err.type = reproc::redirect::pipe;
    } else {
        options.redirect.out.type = reproc::redirect::parent;
        options.redirect.err.type = reproc::redirect::parent;
    }

    if (working_dir) {
        options.working_directory = working_dir->c_str();
    }

    std::vector<std::string> env_strings;
    std::vector<const char *> env_ptrs;
    if (env) {
        options.env.behavior = reproc::env::extend;
        for (const auto &[key, value] : *env) {
            env_strings.push_back(key + "=" + value);
        }
        for (const auto &s : env_strings) {
            env_ptrs.push_back(s.c_str());
        }
        env_ptrs.push_back(nullptr);
        options.env.extra = env_ptrs.data();
    }

    int status = 0;
    std::error_code ec;
    if (capture_output) {
        reproc::sink::string sink(captured);
        std::tie(status, ec) = reproc::run(args, options, sink, sink);
    } else {
        std::tie(status, ec) = reproc::run(args, options);
    }

    if (ec)
        return std::pair<int, std::string>{-1, captured};
    return std::pair<int, std::string>{status, captured};
}
} // namespace catalyst
