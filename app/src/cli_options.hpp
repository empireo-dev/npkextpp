#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace npkextpp {

inline constexpr int kExitOk = 0;
inline constexpr int kExitEntriesFailed = 1;
inline constexpr int kExitBadInput = 2;
inline constexpr int kExitInterrupted = 130;

struct CliOptions {
    std::optional<std::filesystem::path> install;   // game installation root
    std::vector<std::string> inputs;                // --in: archive paths / names / paths inside the installation
    std::vector<std::filesystem::path> resdb;       // --resdb: manifests to use instead of the installation's
    std::vector<std::string> unpack;                // --unpack: only archives with these names
    std::optional<std::size_t> limit;               // --limit: first N entries of each archive
    std::optional<std::filesystem::path> out;       // output folder
    std::optional<std::string> entry;               // --entry: extract one file by its path in the tree
    bool scan = false;                              // --scan: walk the installation instead of the built-in list
    bool overwrite = false;                         // --overwrite: rewrite files already in the output folder
    bool keep_overridden = false;
    bool dry_run = false;
    bool verify = true;
    bool verbose = false;
    bool show_licenses = false;
};

// Parses argv. Returns nullopt after printing help / an error; `exit_code` is
// then what main() should return.
[[nodiscard]] std::optional<CliOptions> parse_cli(int argc, char** argv, int& exit_code);

} // namespace npkextpp
