#include "npk/decoder.hpp"

#include "npk/hash.hpp"

#include <libdeflate.h>
#include <lz4.h>
#include <zstd.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <new>
#include <string>

namespace npk {

namespace {

// The read buffer of Decoder::read() keeps its memory between calls, but not
// after an unusually large entry.
constexpr std::size_t kKeepBufferBytes = std::size_t{64} << 20;

constexpr unsigned kXorStart = 6;

std::string size_message(std::size_t got, std::size_t expected)
{
    return "decoded " + std::to_string(got) + " bytes, expected " + std::to_string(expected);
}

} // namespace

void decrypt(std::span<std::byte> stored) noexcept
{
    const std::size_t n = std::min(stored.size(), kEncryptedPrefix);
    for (std::size_t i = 0; i < n; ++i) {
        stored[i] ^= static_cast<std::byte>((i + kXorStart) & 0xFFu);
    }
}

struct Decoder::Impl {
    ZSTD_DCtx* zstd = nullptr;
    libdeflate_decompressor* deflate = nullptr;
    std::vector<std::byte> stored;

    Impl()
        : zstd(ZSTD_createDCtx())
        , deflate(libdeflate_alloc_decompressor())
    {
        if (zstd == nullptr || deflate == nullptr) {
            release();
            throw std::bad_alloc();
        }
    }
    ~Impl() { release(); }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    void release() noexcept
    {
        libdeflate_free_decompressor(deflate);
        ZSTD_freeDCtx(zstd);
        deflate = nullptr;
        zstd = nullptr;
    }
};

Decoder::Decoder() : impl_(std::make_unique<Impl>()) {}
Decoder::~Decoder() = default;
Decoder::Decoder(Decoder&&) noexcept = default;
Decoder& Decoder::operator=(Decoder&&) noexcept = default;

Result<void> Decoder::decompress(std::span<const std::byte> data, const Entry& entry, std::span<std::byte> out)
{
    if (out.size() != entry.raw_size) {
        return make_error(Errc::invalid_input, "output buffer size does not match the entry's raw size");
    }
    if (entry.raw_size == 0) return {};   // an empty file, whatever the mode says

    switch (entry.compression()) {
    case Compression::none:
        if (data.size() != out.size()) return make_error(Errc::size_mismatch, size_message(data.size(), out.size()));
        std::memcpy(out.data(), data.data(), out.size());
        return {};

    case Compression::zlib: {
        std::size_t consumed = 0;   // trailing bytes after the stream are ignored, like zlib does
        std::size_t produced = 0;
        const libdeflate_result result = libdeflate_zlib_decompress_ex(
            impl_->deflate, data.data(), data.size(), out.data(), out.size(), &consumed, &produced);
        switch (result) {
        case LIBDEFLATE_SUCCESS:
            if (produced != out.size()) return make_error(Errc::size_mismatch, size_message(produced, out.size()));
            return {};
        case LIBDEFLATE_INSUFFICIENT_SPACE:
            return make_error(Errc::size_mismatch, "decoded more than the expected " + std::to_string(out.size()) +
                                                       " bytes");
        case LIBDEFLATE_BAD_DATA:
            return make_error(Errc::decompress_failed, "zlib decompression failed: invalid data");
        default:
            return make_error(Errc::decompress_failed, "zlib decompression failed");
        }
    }

    case Compression::lz4: {
        if (data.size() > static_cast<std::size_t>(INT_MAX) || out.size() > static_cast<std::size_t>(INT_MAX)) {
            return make_error(Errc::unsupported, "entry too large for LZ4");
        }
        const int produced = LZ4_decompress_safe(reinterpret_cast<const char*>(data.data()),
                                                 reinterpret_cast<char*>(out.data()), static_cast<int>(data.size()),
                                                 static_cast<int>(out.size()));
        if (produced < 0) {
            return make_error(Errc::decompress_failed,
                              "lz4 decompression failed: corrupt input (error " + std::to_string(produced) + ")");
        }
        if (static_cast<std::size_t>(produced) != out.size()) {
            return make_error(Errc::size_mismatch, size_message(static_cast<std::size_t>(produced), out.size()));
        }
        return {};
    }

    case Compression::zstd: {
        const std::size_t produced = ZSTD_decompressDCtx(impl_->zstd, out.data(), out.size(), data.data(), data.size());
        if (ZSTD_isError(produced)) {
            return make_error(Errc::decompress_failed,
                              std::string("zstd decompression failed: ") + ZSTD_getErrorName(produced));
        }
        if (produced != out.size()) return make_error(Errc::size_mismatch, size_message(produced, out.size()));
        return {};
    }
    }
    return make_error(Errc::unsupported,
                      "unknown compression mode " + std::to_string(static_cast<unsigned>(entry.compression())));
}

Result<void> Decoder::decode(std::span<std::byte> stored, const Entry& entry, std::span<std::byte> out, bool verify)
{
    if (verify && murmur3_32(stored) != entry.crc_packed) {
        return make_error(Errc::crc_packed_mismatch, "stored data is damaged (crc_packed does not match)");
    }
    if (entry.encrypted()) decrypt(stored);
    if (auto result = decompress(stored, entry, out); !result) return result;
    if (verify && murmur3_32(out) != entry.crc_raw) {
        return make_error(Errc::crc_raw_mismatch, "decoded data is wrong (crc_raw does not match)");
    }
    return {};
}

Result<std::vector<std::byte>> Decoder::read(const Archive& archive, const Entry& entry, bool verify)
{
    std::vector<std::byte>& stored = impl_->stored;
    stored.resize(entry.packed_size);

    Result<std::vector<std::byte>> result = archive.read_stored(entry, stored).and_then([&]() -> Result<std::vector<std::byte>> {
        std::vector<std::byte> out(entry.raw_size);
        if (auto decoded = decode(stored, entry, out, verify); !decoded) return std::unexpected(std::move(decoded.error()));
        return out;
    });

    if (stored.capacity() > kKeepBufferBytes) {
        stored = {};
    }
    return result;
}

} // namespace npk
