#include "npk/archive_update.hpp"

#include "npk/archive.hpp"
#include "npk/decoder.hpp"
#include "npk/encoder.hpp"
#include "npk/hash.hpp"
#include "npk/paths.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <vector>

namespace fs = std::filesystem;

namespace npk {

namespace {

constexpr std::uint64_t kAlign = 4;   // the game's packer starts every file on a 4-byte boundary

std::uint64_t aligned(std::uint64_t offset)
{
    return (offset + kAlign - 1) / kAlign * kAlign;
}

void put32(std::vector<std::byte>& out, std::uint32_t value)
{
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xFFu));
}

std::vector<std::byte> toc_bytes(const std::vector<Entry>& entries)
{
    std::vector<std::byte> out;
    out.reserve(entries.size() * Entry::kDiskSize);
    for (const Entry& e : entries) {
        for (const std::uint32_t v : {e.id, e.offset, e.packed_size, e.raw_size, e.crc_packed, e.crc_raw, e.flags}) put32(out, v);
    }
    return out;
}

bool write_at(std::fstream& file, std::uint64_t offset, std::span<const std::byte> data)
{
    file.seekp(static_cast<std::streamoff>(offset));
    file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(file);
}

std::string with_backslashes(std::string_view path)
{
    std::string out(path);
    std::ranges::replace(out, '/', '\\');
    while (!out.empty() && out.front() == '\\') out.erase(0, 1);
    return out;
}

} // namespace

Result<FoundEntry> find_entry(const fs::path& path, std::string_view wanted)
{
    auto archive = Archive::open(path);
    if (!archive) return std::unexpected(std::move(archive.error()));
    const std::string full = with_backslashes(wanted);
    // The whole path, then without its first folder, and so on.
    for (std::size_t start = 0; start < full.size();) {
        const std::string tail = full.substr(start);
        if (archive->find(tail) != nullptr) return FoundEntry{path_id(tail), tail};
        const std::size_t slash = full.find('\\', start);
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return make_error(Errc::not_found, std::string(wanted) + " is not in " + to_utf8(path));
}

Result<Replacement> replace_entry(const fs::path& path, std::uint32_t id, std::span<const std::byte> content)
{
    Replacement result;
    std::vector<Entry> entries;
    std::uint32_t toc_offset = 0;
    {
        auto archive = Archive::open(path);
        if (!archive) return std::unexpected(std::move(archive.error()));
        std::vector<std::byte> header(Archive::kHeaderSize);
        if (auto read = archive->file().read_at(0, header); !read) return std::unexpected(std::move(read.error()));
        auto parsed = Archive::parse_header(header);
        if (!parsed) return std::unexpected(std::move(parsed.error()));
        toc_offset = parsed->toc_offset;
        entries = archive->entries();
        result.size_before = archive->file_size();
    }
    const auto row = std::ranges::find(entries, id, &Entry::id);
    if (row == entries.end()) return make_error(Errc::not_found, "the file is not in " + to_utf8(path));
    const std::uint64_t toc_end = std::uint64_t{toc_offset} + entries.size() * Entry::kDiskSize;
    if (result.size_before != toc_end) {
        return make_error(Errc::unsupported, to_utf8(path) + " has data after its table of contents; not changed");
    }
    result.before = *row;

    auto stored = encode(content, row->flags);
    if (!stored) return std::unexpected(std::move(stored.error()));

    // Room where the old version is: up to the next file's bytes or the table.
    // Shared with another row (the same bytes listed twice): no room.
    std::uint64_t room_end = toc_offset;
    bool shared = false;
    for (const Entry& other : entries) {
        if (&other == &*row) continue;
        if (other.offset == row->offset && other.packed_size != 0) shared = true;
        if (other.offset > row->offset && other.offset < room_end) room_end = other.offset;
    }
    const std::uint64_t room = room_end > row->offset ? room_end - row->offset : 0;

    std::uint64_t offset = row->offset;
    std::uint64_t new_toc = toc_offset;
    result.in_place = !shared && stored->size() <= room;
    if (!result.in_place) {
        offset = aligned(toc_offset);
        new_toc = aligned(offset + stored->size());
    }
    if (new_toc + entries.size() * Entry::kDiskSize > std::numeric_limits<std::uint32_t>::max()) {
        return make_error(Errc::unsupported, to_utf8(path) + " would grow over 4 GiB");
    }
    *row = make_entry(id, static_cast<std::uint32_t>(offset), content, *stored, row->flags);
    result.after = *row;

    {
        std::fstream file(long_path(path), std::ios::in | std::ios::out | std::ios::binary);
        if (!file) return make_error(Errc::io_error, "cannot open " + to_utf8(path) + " for writing (is the game running?)");
        const std::vector<std::byte> zeros(kAlign, std::byte{0});
        bool ok = write_at(file, offset, *stored);
        if (!result.in_place) {
            ok = ok && write_at(file, offset + stored->size(), std::span(zeros).first(new_toc - offset - stored->size()));
            std::vector<std::byte> pointer;
            put32(pointer, static_cast<std::uint32_t>(new_toc));
            ok = ok && write_at(file, Archive::kTocOffsetOffset, pointer);
        }
        ok = ok && write_at(file, new_toc, toc_bytes(entries));
        file.flush();
        if (!ok || !file) return make_error(Errc::io_error, "writing " + to_utf8(path) + " failed; the archive may be damaged");
    }

    // Read it back.
    auto archive = Archive::open(path);
    if (!archive) return std::unexpected(std::move(archive.error()));
    result.size_after = archive->file_size();
    const Entry* written = archive->find(id);
    if (written == nullptr) return make_error(Errc::bad_format, "the replaced file is missing after writing");
    Decoder decoder;
    auto decoded = decoder.read(*archive, *written, true);
    if (!decoded) return make_error(decoded.error().code, "reading the replaced file back failed: " + decoded.error().message);
    if (decoded->size() != content.size() || !std::equal(decoded->begin(), decoded->end(), content.begin())) {
        return make_error(Errc::crc_raw_mismatch, "the replaced file reads back differently");
    }
    return result;
}

} // namespace npk
