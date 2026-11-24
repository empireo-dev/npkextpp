#include "npk/archive.hpp"

#include "bytes.hpp"
#include "npk/hash.hpp"
#include "npk/paths.hpp"

#include <algorithm>
#include <array>
#include <cstdio>

namespace npk {

namespace {

constexpr std::array<std::byte, 4> kMagic{std::byte{'N'}, std::byte{'X'}, std::byte{'P'}, std::byte{'K'}};

std::string printable_magic(std::span<const std::byte> bytes)
{
    std::string out;
    for (std::size_t i = 0; i < bytes.size() && i < 4; ++i) {
        const auto c = static_cast<unsigned char>(bytes[i]);
        if (c >= 0x20 && c < 0x7F && c != '\\') {
            out += static_cast<char>(c);
        } else {
            char hex[8];
            std::snprintf(hex, sizeof hex, "\\x%02x", c);
            out += hex;
        }
    }
    return out;
}

} // namespace

Result<Archive::Header> Archive::parse_header(std::span<const std::byte> header)
{
    if (header.size() < kHeaderSize) return make_error(Errc::bad_format, "too small to be an NXPK archive");
    if (!std::ranges::equal(header.first(4), kMagic)) {
        return make_error(Errc::bad_format, "not an NXPK archive (starts with '" + printable_magic(header) + "')");
    }
    return Header{detail::load_le32(header.data() + kEntryCountOffset),
                  detail::load_le32(header.data() + kTocOffsetOffset)};
}

Result<std::vector<Entry>> Archive::parse_toc(std::span<const std::byte> toc, std::uint32_t entry_count)
{
    const std::uint64_t needed = std::uint64_t{entry_count} * Entry::kDiskSize;
    if (toc.size() < needed) return make_error(Errc::bad_format, "table of contents is truncated");

    std::vector<Entry> entries(entry_count);
    const std::byte* row = toc.data();
    for (Entry& entry : entries) {
        entry.id = detail::load_le32(row + 0);
        entry.offset = detail::load_le32(row + 4);
        entry.packed_size = detail::load_le32(row + 8);
        entry.raw_size = detail::load_le32(row + 12);
        entry.crc_packed = detail::load_le32(row + 16);
        entry.crc_raw = detail::load_le32(row + 20);
        entry.flags = detail::load_le32(row + 24);
        row += Entry::kDiskSize;
    }
    return entries;
}

Result<Archive> Archive::open(const std::filesystem::path& path)
{
    const auto fail = [&path](Error error) {
        error.message = to_utf8(path) + ": " + error.message;
        return std::unexpected(std::move(error));
    };

    Archive archive;
    archive.path_ = path;
    auto file = InputFile::open(path);
    if (!file) return std::unexpected(std::move(file.error()));
    archive.file_ = std::move(*file);

    if (archive.file_.size() < kHeaderSize) return fail({Errc::bad_format, "too small to be an NXPK archive"});
    std::array<std::byte, kHeaderSize> header_bytes{};
    if (auto read = archive.file_.read_at(0, header_bytes); !read) return fail(std::move(read.error()));
    auto header = parse_header(header_bytes);
    if (!header) return fail(std::move(header.error()));

    const std::uint64_t toc_size = std::uint64_t{header->entry_count} * Entry::kDiskSize;
    if (std::uint64_t{header->toc_offset} + toc_size > archive.file_.size()) {
        return fail({Errc::bad_format, "table of contents runs past the end of the file"});
    }
    std::vector<std::byte> toc(static_cast<std::size_t>(toc_size));
    if (auto read = archive.file_.read_at(header->toc_offset, toc); !read) return fail(std::move(read.error()));
    auto entries = parse_toc(toc, header->entry_count);
    if (!entries) return fail(std::move(entries.error()));

    archive.entries_ = std::move(*entries);
    archive.sorted_by_id_ = std::ranges::is_sorted(archive.entries_, {}, &Entry::id);
    return archive;
}

const Entry* Archive::find(std::uint32_t id) const noexcept
{
    if (sorted_by_id_) {
        const auto it = std::ranges::lower_bound(entries_, id, {}, &Entry::id);
        return (it != entries_.end() && it->id == id) ? &*it : nullptr;
    }
    const auto it = std::ranges::find(entries_, id, &Entry::id);
    return it != entries_.end() ? &*it : nullptr;
}

const Entry* Archive::find(std::string_view inner_path) const noexcept
{
    return find(path_id(inner_path));
}

Result<void> Archive::read_stored(const Entry& entry, std::span<std::byte> out) const
{
    if (out.size() != entry.packed_size) {
        return make_error(Errc::invalid_input, "buffer size does not match the entry's packed size");
    }
    if (std::uint64_t{entry.offset} + entry.packed_size > file_.size()) {
        return make_error(Errc::bad_format, "data runs past the end of " + to_utf8(path_.filename()));
    }
    return file_.read_at(entry.offset, out);
}

} // namespace npk
