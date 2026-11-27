#pragma once

#include "npk/error.hpp"

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace npk {

// The game's resource manifest: a 16-byte header starting with "CCZ!" followed
// by a zlib stream of UTF-8 text lines "<flag> <path>" (backslash-separated
// paths, original case). Returns every listed path, in file order.
[[nodiscard]] Result<std::vector<std::string>> read_resdb(const std::filesystem::path& path);
[[nodiscard]] Result<std::vector<std::string>> parse_resdb(std::span<const std::byte> data);

inline constexpr std::size_t kResdbHeaderSize = 16;

} // namespace npk
