#include "npk/archive_update.hpp"

#include "npk/archive.hpp"
#include "npk/decoder.hpp"
#include "npk/encoder.hpp"
#include "npk/hash.hpp"
#include "npk/paths.hpp"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <optional>
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

Result<ReplaceResult> replace_entries(const fs::path& path, std::span<const Change> changes)
{
    ReplaceResult result;
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
    const std::uint64_t toc_end = std::uint64_t{toc_offset} + entries.size() * Entry::kDiskSize;
    if (result.size_before != toc_end) {
        return make_error(Errc::unsupported, to_utf8(path) + " has data after its table of contents; not changed");
    }
    std::vector<std::size_t> rows;   // of each change, into entries
    for (const Change& change : changes) {
        const auto row = std::ranges::find(entries, change.id, &Entry::id);
        if (row == entries.end()) return make_error(Errc::not_found, "a file to replace is not in " + to_utf8(path));
        const auto index = static_cast<std::size_t>(row - entries.begin());
        if (std::ranges::find(rows, index) != rows.end()) {
            return make_error(Errc::invalid_input, "the same file is to be replaced twice");
        }
        rows.push_back(index);
    }

    std::fstream file(long_path(path), std::ios::in | std::ios::out | std::ios::binary);
    if (!file) return make_error(Errc::io_error, "cannot open " + to_utf8(path) + " for writing (is the game running?)");
    const std::vector<std::byte> zeros(kAlign, std::byte{0});
    std::uint64_t end = toc_offset;   // where the next file that does not fit goes
    std::optional<Error> failure;

    for (std::size_t c = 0; c < changes.size() && !failure; ++c) {
        Entry& row = entries[rows[c]];
        Replacement done;
        done.before = row;
        auto content = changes[c].load();
        if (!content) {
            failure = std::move(content.error());
            break;
        }
        if (content->size() == row.raw_size && murmur3_32(*content) == row.crc_raw) {
            done.after = row;
            done.unchanged = true;
            result.files.push_back(done);
            continue;
        }
        auto stored = encode(*content, row.flags);
        if (!stored) {
            failure = std::move(stored.error());
            break;
        }
        // Room where the old version is: up to the next file's bytes or the old
        // table. Shared with another row (the same bytes listed twice): none.
        std::uint64_t room_end = toc_offset;
        bool shared = false;
        for (const Entry& other : entries) {
            if (&other == &row) continue;
            if (other.offset == row.offset && other.packed_size != 0) shared = true;
            if (other.offset > row.offset && other.offset < room_end) room_end = other.offset;
        }
        const std::uint64_t room = room_end > row.offset ? room_end - row.offset : 0;
        done.in_place = !shared && row.offset < toc_offset && stored->size() <= room;
        std::uint64_t offset = row.offset;
        if (!done.in_place) {
            offset = aligned(end);
            if (aligned(offset + stored->size()) + entries.size() * Entry::kDiskSize > std::numeric_limits<std::uint32_t>::max()) {
                failure = Error{Errc::unsupported, to_utf8(path) + " would grow over 4 GiB"};
                break;
            }
            if (!write_at(file, end, std::span(zeros).first(offset - end))) break;
        }
        if (!write_at(file, offset, *stored)) break;
        if (!done.in_place) end = offset + stored->size();
        row = make_entry(row.id, static_cast<std::uint32_t>(offset), *content, *stored, row.flags);
        done.after = row;
        result.files.push_back(done);
    }

    // The table: where it was, or after the files that went to the end.
    const std::uint64_t new_toc = end == toc_offset ? toc_offset : aligned(end);
    bool ok = static_cast<bool>(file);
    if (ok && new_toc != toc_offset) {
        std::vector<std::byte> pointer;
        put32(pointer, static_cast<std::uint32_t>(new_toc));
        ok = write_at(file, end, std::span(zeros).first(new_toc - end)) && write_at(file, Archive::kTocOffsetOffset, pointer);
    }
    ok = ok && write_at(file, new_toc, toc_bytes(entries));
    file.flush();
    file.close();
    if (!ok || file.fail()) return make_error(Errc::io_error, "writing " + to_utf8(path) + " failed; the archive may be damaged");
    if (failure) {
        failure->message = "stopped after " + std::to_string(result.files.size()) + " of " +
                           std::to_string(changes.size()) + " files (those are replaced): " + failure->message;
        return std::unexpected(std::move(*failure));
    }

    // Read every replaced file back.
    auto archive = Archive::open(path);
    if (!archive) return std::unexpected(std::move(archive.error()));
    result.size_after = archive->file_size();
    Decoder decoder;
    for (std::size_t c = 0; c < result.files.size(); ++c) {
        if (result.files[c].unchanged) continue;
        const Entry* written = archive->find(result.files[c].after.id);
        if (written == nullptr) return make_error(Errc::bad_format, "a replaced file is missing after writing");
        auto decoded = decoder.read(*archive, *written, true);
        auto content = changes[c].load();
        if (!decoded || !content || *decoded != *content) {
            return make_error(Errc::crc_raw_mismatch, "a replaced file reads back differently" +
                                                          (decoded ? std::string() : ": " + decoded.error().message));
        }
    }
    return result;
}

Result<ReplaceResult> replace_entry(const fs::path& path, std::uint32_t id, std::span<const std::byte> content)
{
    const std::vector<std::byte> copy(content.begin(), content.end());
    const Change change{id, [&copy]() -> Result<std::vector<std::byte>> { return copy; }};
    return replace_entries(path, std::span(&change, 1));
}

Result<std::vector<FolderFile>> folder_files(const fs::path& folder)
{
    std::error_code error;
    if (!fs::is_directory(folder, error)) return make_error(Errc::not_found, "folder not found: " + to_utf8(folder));
    std::vector<FolderFile> files;
    fs::recursive_directory_iterator it(folder, error);
    for (; !error && it != fs::recursive_directory_iterator(); it.increment(error)) {
        const fs::path relative = it->path().lexically_relative(folder);
        const std::string first = to_utf8(*relative.begin());
        std::error_code type_error;
        if (it->is_directory(type_error) && relative.begin() != relative.end() && std::next(relative.begin()) == relative.end() &&
            (first == "_archives" || first == "_overridden")) {
            it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(type_error)) continue;
        std::string inner = with_backslashes(to_utf8(relative));
        FolderFile entry{it->path(), inner, path_id(inner)};
        // _unnamed/0123abcd.ext: the id itself.
        if (first == "_unnamed" && std::next(relative.begin()) != relative.end() &&
            std::next(std::next(relative.begin())) == relative.end()) {
            const std::string name = to_utf8(relative.filename());
            const std::string digits = name.substr(0, 8);
            if (digits.size() == 8 && std::ranges::all_of(digits, [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }) &&
                (name.size() == 8 || name[8] == '.')) {
                entry.id = static_cast<std::uint32_t>(std::stoul(digits, nullptr, 16));
            }
        }
        files.push_back(std::move(entry));
    }
    if (error) return make_error(Errc::io_error, "cannot read the folder " + to_utf8(folder) + ": " + error.message());
    std::ranges::sort(files, {}, &FolderFile::inner_path);
    return files;
}

} // namespace npk
