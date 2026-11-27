#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace npk {

// --- Text and path conversions -------------------------------------------------
// Inside npk all names are UTF-8 std::strings with '/' separators ("generic"
// form). These convert at the std::filesystem boundary, so the same code works
// with wide Windows paths.

[[nodiscard]] std::string to_utf8(const std::filesystem::path& path);   // generic form, '/'
[[nodiscard]] std::filesystem::path from_utf8(std::string_view text);

// ASCII-only lower case. Resource paths are ASCII; other bytes are kept.
[[nodiscard]] std::string lower_ascii(std::string_view text);
[[nodiscard]] bool iequals_ascii(std::string_view a, std::string_view b) noexcept;
[[nodiscard]] bool istarts_with_ascii(std::string_view text, std::string_view prefix) noexcept;

// The elements of a path in generic form ("/", "home", "x.npk"), as UTF-8.
[[nodiscard]] std::vector<std::string> path_parts(const std::filesystem::path& path);

// --- Output names ---------------------------------------------------------------

// A manifest path ("a\\b\\c.dds") as a relative output path ("a/b/c.dds") that
// cannot leave the output folder: empty and "." parts are dropped; "..", a drive
// or other ':' makes it unusable (nullopt).
[[nodiscard]] std::optional<std::string> safe_relative_path(std::string_view inner_path);

// "Documents/res/ui.npk" -> "Documents_res_ui.npk": runs of '\\', '/' and ':'
// become one '_', leading and trailing '_' are removed.
[[nodiscard]] std::string folder_tag(std::string_view label);

// --- Windows long paths -----------------------------------------------------------

// "C:\\out" -> "\\\\?\\C:\\out", "\\\\server\\share" -> "\\\\?\\UNC\\server\\share";
// already extended paths are returned unchanged. Pure string function.
[[nodiscard]] std::string extended_length_path(std::string_view absolute);

// What to hand to the OS when opening a file. On Windows, the absolute path in
// extended-length form, so paths longer than 260 characters work without the
// system-wide long path setting. Elsewhere the path unchanged.
[[nodiscard]] std::filesystem::path long_path(const std::filesystem::path& path);

} // namespace npk
