#pragma once

#include "npk/error.hpp"
#include "npk/install.hpp"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace npk {

// Turns the values given on the command line into archive files. Each value is:
//   * a path to an archive file                          path/to/ui.npk
//   * with an installation: an archive name, searched    ui.npk, ui, 'res_dlc_*'
//     in every mount folder of the installation
//   * with an installation: a path relative to it        res/ui.npk, 'Documents/res/*.npk'
//     (selects specific mount folders; wildcards allowed)
// Duplicates are dropped, the order is kept.
[[nodiscard]] Result<std::vector<std::filesystem::path>> resolve_inputs(std::span<const std::string> inputs,
                                                                         const Installation* installation);

// Keeps the archives whose file name matches any of the patterns: a file name
// with or without ".npk", wildcards allowed ('ui.npk', 'ui', 'res_dlc_*').
// Ignores case. A pattern matching nothing is an error that suggests close names.
[[nodiscard]] Result<std::vector<std::filesystem::path>> filter_by_name(std::span<const std::filesystem::path> archives,
                                                                         std::span<const std::string> patterns);

// Shell-style wildcard match, like Python's fnmatch.fnmatchcase: '*', '?',
// '[abc]', '[a-z]', '[!abc]'. Case-sensitive.
[[nodiscard]] bool wildcard_match(std::string_view text, std::string_view pattern);

// The closest candidates to `word` (like Python's difflib.get_close_matches):
// at most `n`, similarity ratio >= cutoff, best first.
[[nodiscard]] std::vector<std::string> close_matches(std::string_view word, std::span<const std::string> candidates,
                                                     std::size_t n = 4, double cutoff = 0.5);

// The files and folders below `root` that match a relative '/' pattern whose
// parts may hold wildcards; "**" matches any number of folders.
[[nodiscard]] std::vector<std::filesystem::path> glob(const std::filesystem::path& root, std::string_view pattern);

} // namespace npk
