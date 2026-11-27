#include "npk/encoder.hpp"

#include "npk/decoder.hpp"
#include "npk/hash.hpp"

#include <libdeflate.h>
#include <lz4.h>
#include <zstd.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <string>

namespace npk {

namespace {

constexpr int kZlibLevel = 6;
constexpr int kZstdLevel = 3;

struct CompressorDeleter {
    void operator()(libdeflate_compressor* c) const noexcept { libdeflate_free_compressor(c); }
};

} // namespace

Result<std::vector<std::byte>> encode(std::span<const std::byte> content, std::uint32_t flags)
{
    if (content.size() > std::numeric_limits<std::uint32_t>::max()) {
        return make_error(Errc::unsupported, "file too large for an archive entry (over 4 GiB)");
    }
    const Entry entry{0, 0, 0, static_cast<std::uint32_t>(content.size()), 0, 0, flags};
    std::vector<std::byte> stored;
    switch (entry.compression()) {
    case Compression::none: stored.assign(content.begin(), content.end()); break;
    case Compression::zlib: {
        std::unique_ptr<libdeflate_compressor, CompressorDeleter> c(libdeflate_alloc_compressor(kZlibLevel));
        if (!c) return make_error(Errc::io_error, "out of memory");
        stored.resize(libdeflate_zlib_compress_bound(c.get(), content.size()));
        const std::size_t n = libdeflate_zlib_compress(c.get(), content.data(), content.size(), stored.data(), stored.size());
        if (n == 0) return make_error(Errc::unsupported, "zlib compression failed");
        stored.resize(n);
        break;
    }
    case Compression::lz4: {
        if (content.size() > static_cast<std::size_t>(LZ4_MAX_INPUT_SIZE)) {
            return make_error(Errc::unsupported, "file too large for LZ4");
        }
        stored.resize(static_cast<std::size_t>(LZ4_compressBound(static_cast<int>(content.size()))));
        const int n = LZ4_compress_default(reinterpret_cast<const char*>(content.data()), reinterpret_cast<char*>(stored.data()),
                                           static_cast<int>(content.size()), static_cast<int>(stored.size()));
        if (n <= 0 && !content.empty()) return make_error(Errc::unsupported, "LZ4 compression failed");
        stored.resize(static_cast<std::size_t>(std::max(n, 0)));
        break;
    }
    case Compression::zstd: {
        stored.resize(ZSTD_compressBound(content.size()));
        const std::size_t n = ZSTD_compress(stored.data(), stored.size(), content.data(), content.size(), kZstdLevel);
        if (ZSTD_isError(n)) return make_error(Errc::unsupported, std::string("zstd compression failed: ") + ZSTD_getErrorName(n));
        stored.resize(n);
        break;
    }
    default:
        return make_error(Errc::unsupported, "unknown compression mode " + std::to_string(flags & 0xFFu));
    }
    if (entry.encrypted()) decrypt(stored);   // the scrambling is its own inverse
    return stored;
}

Entry make_entry(std::uint32_t id, std::uint32_t offset, std::span<const std::byte> content,
                 std::span<const std::byte> stored, std::uint32_t flags) noexcept
{
    return Entry{id,
                 offset,
                 static_cast<std::uint32_t>(stored.size()),
                 static_cast<std::uint32_t>(content.size()),
                 murmur3_32(stored),
                 murmur3_32(content),
                 flags};
}

} // namespace npk
