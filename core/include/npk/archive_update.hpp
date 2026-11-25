#pragma once

// Changing files inside an existing archive without rebuilding it.
//
// Every row of the table of contents says where its file's bytes are, so files
// need not be in any order and nothing has to move:
// - the new version fits where the old one was (up to the next file, or the
//   table): it is written over it;
// - it does not: it is written where the table was, and the table after it.
// The header then points at the table. The archive keeps its other files'
// bytes untouched. Not crash-safe: an interrupted write leaves the archive
// damaged (keep a copy of an archive you cannot download again).

#include "npk/entry.hpp"
#include "npk/error.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace npk {

// The file `path` in `archive`, accepting '/' or '\\' and paths that start
// with folders above the archive ("res/ui/login/bg.png" for login\\bg.png in
// ui.npk): the longest tail of the path that is in the archive (Errc::not_found
// if none is). The path is case-sensitive, as the game hashes it.
struct FoundEntry {
    std::uint32_t id = 0;
    std::string inner_path;   // backslash separated, as hashed
};

[[nodiscard]] Result<FoundEntry> find_entry(const std::filesystem::path& archive, std::string_view path);

struct Replacement {
    Entry before;
    Entry after;
    bool in_place = false;           // written over the old version
    std::uint64_t size_before = 0;   // of the archive
    std::uint64_t size_after = 0;
};

// Replaces the file `id` of the archive with `content`, compressed and
// scrambled the way the old version was (its flags), then reads it back to
// check it.
[[nodiscard]] Result<Replacement> replace_entry(const std::filesystem::path& archive, std::uint32_t id,
                                                std::span<const std::byte> content);

} // namespace npk
