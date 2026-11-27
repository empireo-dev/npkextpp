#pragma once

#include "npk/error.hpp"
#include "npk/plan.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace npk::pipeline {

inline constexpr std::string_view kIndexFolder = kIndexFolderName;

// Writes the _archives/ index from a Plan whose entries carry their results, in
// the same format as npkext:
//   index.csv      one row per archive ever unpacked into the output folder
//                  (rows of earlier runs are kept, this run's replace them)
//   mounts.csv     the mount folders and their priority
//   README.txt     what the columns mean
//   <archive>.csv  one record per archive, rows in table-of-contents order
// Entries not processed (--limit, or a stopped run) are recorded as "skipped".
[[nodiscard]] Result<void> write_index(const Plan& plan, const std::filesystem::path& output_root);

// CSV helpers (as Python's csv module: minimal quoting, \r\n line ends).
[[nodiscard]] std::string csv_row(const std::vector<std::string>& fields);
[[nodiscard]] std::vector<std::vector<std::string>> parse_csv(std::string_view text);

} // namespace npk::pipeline
