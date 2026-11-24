#include "npk/resdb.hpp"

#include "npk/input_file.hpp"
#include "npk/paths.hpp"

#include <libdeflate.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string_view>

namespace npk {

namespace {

constexpr char kMagic[4] = {'C', 'C', 'Z', '!'};
constexpr std::size_t kMaxText = std::size_t{1} << 31;   // far above any real manifest

struct DecompressorDeleter {
    void operator()(libdeflate_decompressor* d) const noexcept { libdeflate_free_decompressor(d); }
};

// zlib stream of unknown decompressed size: grow the buffer until it fits.
Result<std::string> inflate_all(std::span<const std::byte> stream)
{
    std::unique_ptr<libdeflate_decompressor, DecompressorDeleter> d(libdeflate_alloc_decompressor());
    if (!d) return make_error(Errc::io_error, "out of memory");

    std::string text;
    std::size_t capacity = std::max<std::size_t>(stream.size() * 8, 1 << 20);
    for (;;) {
        text.resize(capacity);
        std::size_t consumed = 0;
        std::size_t produced = 0;
        const libdeflate_result result = libdeflate_zlib_decompress_ex(d.get(), stream.data(), stream.size(),
                                                                       text.data(), text.size(), &consumed, &produced);
        if (result == LIBDEFLATE_SUCCESS) {
            text.resize(produced);
            return text;
        }
        if (result != LIBDEFLATE_INSUFFICIENT_SPACE || capacity >= kMaxText) {
            return make_error(Errc::bad_format, "cannot decompress: invalid zlib data");
        }
        capacity *= 2;
    }
}

} // namespace

Result<std::vector<std::string>> parse_resdb(std::span<const std::byte> data)
{
    if (data.size() < sizeof kMagic || std::memcmp(data.data(), kMagic, sizeof kMagic) != 0) {
        return make_error(Errc::bad_format, "not a resdb manifest (does not start with CCZ!)");
    }
    if (data.size() < kResdbHeaderSize) return make_error(Errc::bad_format, "resdb header is truncated");

    auto text = inflate_all(data.subspan(kResdbHeaderSize));
    if (!text) return std::unexpected(std::move(text.error()));

    // Lines end with \n, \r\n or \r. Each is "<flag> <path>"; the flag is ignored.
    std::vector<std::string> paths;
    const std::string_view all = *text;
    std::size_t start = 0;
    while (start < all.size()) {
        std::size_t end = all.find_first_of("\r\n", start);
        if (end == std::string_view::npos) end = all.size();
        const std::string_view line = all.substr(start, end - start);
        if (const auto space = line.find(' '); space != std::string_view::npos && space + 1 < line.size()) {
            paths.emplace_back(line.substr(space + 1));
        }
        start = end + 1;
        if (end < all.size() && all[end] == '\r' && start < all.size() && all[start] == '\n') ++start;
    }
    return paths;
}

Result<std::vector<std::string>> read_resdb(const std::filesystem::path& path)
{
    auto data = InputFile::read_all(path);
    if (!data) return std::unexpected(std::move(data.error()));
    auto paths = parse_resdb(*data);
    if (!paths) paths.error().message = to_utf8(path) + ": " + paths.error().message;
    return paths;
}

} // namespace npk
