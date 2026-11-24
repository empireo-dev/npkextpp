#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace npkextpp {

struct CliOptions {
    std::filesystem::path install;      // game installation root
    std::filesystem::path out;          // output folder
    std::vector<std::string> only;      // --in: archive names / substrings
    std::optional<std::string> entry;   // --entry: extract one file by path
    bool keep_overridden = false;
    bool verify = true;
    bool verbose = false;
    bool show_licenses = false;
};

// Parses argv. Returns nullopt after printing help / an error; `exit_code` is
// then what main() should return.
[[nodiscard]] std::optional<CliOptions> parse_cli(int argc, char** argv, int& exit_code);

} // namespace npkextpp
