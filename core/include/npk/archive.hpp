#pragma once

#include "npk/entry.hpp"
#include "npk/error.hpp"
#include "npk/input_file.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace npk {

// An opened NXPK archive: header + table of contents. Stored bytes are read on
// demand; read_stored() is safe to call from several threads.
//
// Layout (little-endian u32s):
//   0x00 "NXPK"  0x04 entry count  0x08..0x13 unused  0x14 TOC offset
//   0x18 stored files back to back ... TOC: entry_count rows of 28 bytes
class Archive {
public:
    static constexpr std::size_t kHeaderSize = 0x18;
    static constexpr std::size_t kEntryCountOffset = 0x04;
    static constexpr std::size_t kTocOffsetOffset = 0x14;

    [[nodiscard]] static Result<Archive> open(const std::filesystem::path& path);

    // Parses the header (the first kHeaderSize bytes) into entry count and TOC offset.
    struct Header {
        std::uint32_t entry_count = 0;
        std::uint32_t toc_offset = 0;
    };
    [[nodiscard]] static Result<Header> parse_header(std::span<const std::byte> header);

    // Parses a table of contents already in memory.
    [[nodiscard]] static Result<std::vector<Entry>> parse_toc(std::span<const std::byte> toc,
                                                              std::uint32_t entry_count);

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] const std::vector<Entry>& entries() const noexcept { return entries_; }
    [[nodiscard]] const InputFile& file() const noexcept { return file_; }
    [[nodiscard]] std::uint64_t file_size() const noexcept { return file_.size(); }

    // Finds an entry by id: binary search (the TOC is sorted by id), with a
    // linear fallback for archives whose TOC is not sorted.
    [[nodiscard]] const Entry* find(std::uint32_t id) const noexcept;
    [[nodiscard]] const Entry* find(std::string_view inner_path) const noexcept;

    // Reads the stored (still encrypted / compressed) bytes of one entry.
    // `out` must be exactly entry.packed_size bytes.
    [[nodiscard]] Result<void> read_stored(const Entry& entry, std::span<std::byte> out) const;

    // Releases the file handle; entries() stays valid.
    void close() noexcept { file_.close(); }

private:
    std::filesystem::path path_;
    InputFile file_;
    std::vector<Entry> entries_;
    bool sorted_by_id_ = true;
};

} // namespace npk
