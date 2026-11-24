#pragma once

#include "npk/error.hpp"

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace npk {

// The game's resource manifest: a 16-byte "CCZ!" header followed by a zlib
// stream of text lines "<flag> <path>". Returns every listed path.
[[nodiscard]] Result<std::vector<std::string>> read_resdb(const std::filesystem::path& path);
[[nodiscard]] Result<std::vector<std::string>> parse_resdb(std::span<const std::byte> data);

} // namespace npk
