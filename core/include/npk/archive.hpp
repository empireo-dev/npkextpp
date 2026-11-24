#pragma once

#include "npk/entry.hpp"
#include "npk/error.hpp"
#include "npk/input_file.hpp"

#include <cstddef>
#include <filesystem>
#include <span>
#include <vector>

namespace npk {

// An opened NXPK archive: header + table of contents. Stored bytes are read on
// demand; read_stored() is safe to call from several threads.
class Archive {
public:
    static constexpr std::uint32_t kMagic = 0x4B50584E; // "NXPK"
    static constexpr std::size_t kHeaderSize = 0x18;

    [[nodiscard]] static Result<Archive> open(const std::filesystem::path& path);

    // Parses a table of contents already in memory (for tests and for callers
    // that have the archive bytes themselves).
    [[nodiscard]] static Result<std::vector<Entry>> parse_toc(std::span<const std::byte> toc,
                                                              std::uint32_t entry_count);

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] const std::vector<Entry>& entries() const noexcept { return entries_; }
    [[nodiscard]] const InputFile& file() const noexcept { return file_; }

    // Finds an entry by id (the TOC is sorted by id).
    [[nodiscard]] const Entry* find(std::uint32_t id) const noexcept;

    // Reads the stored (still encrypted / compressed) bytes of one entry.
    [[nodiscard]] Result<void> read_stored(const Entry& entry, std::span<std::byte> out) const;

private:
    std::filesystem::path path_;
    InputFile file_;
    std::vector<Entry> entries_;
};

} // namespace npk
