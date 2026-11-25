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
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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
    bool in_place = false;    // written over the old version
    bool unchanged = false;   // the new version is the same file: nothing written
};

struct ReplaceResult {
    std::vector<Replacement> files;   // in the order of the changes
    std::uint64_t size_before = 0;    // of the archive
    std::uint64_t size_after = 0;
};

// One file to replace: its id and how to get the new version (read when its
// turn comes, so many large files need not be in memory together).
struct Change {
    std::uint32_t id = 0;
    std::function<Result<std::vector<std::byte>>()> load;
};

// Replaces files of the archive, each compressed and scrambled the way its old
// version was (its flags). A new version identical to the old one (same size
// and checksum) is left alone. The table of contents is written once, after
// all the files, then every replaced file is read back and compared. Every id
// must be in the archive, each at most once. If a file cannot be loaded or
// encoded, the ones before it stay replaced and the error says which.
[[nodiscard]] Result<ReplaceResult> replace_entries(const std::filesystem::path& archive,
                                                    std::span<const Change> changes);

// One file: replace_entries() with new content already in memory.
[[nodiscard]] Result<ReplaceResult> replace_entry(const std::filesystem::path& archive, std::uint32_t id,
                                                  std::span<const std::byte> content);

// The files of a folder that has the layout of an archive (as unpacked):
// paths relative to the folder are the paths in the archive, and
// _unnamed/<8 hex digits>[.ext] stands for the id. _archives/ and _overridden/
// are left out.
struct FolderFile {
    std::filesystem::path file;
    std::string inner_path;   // backslash separated; for an unnamed file, its name as in the folder
    std::uint32_t id = 0;
};
[[nodiscard]] Result<std::vector<FolderFile>> folder_files(const std::filesystem::path& folder);

} // namespace npk
