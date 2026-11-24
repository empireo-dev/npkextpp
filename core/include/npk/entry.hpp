#pragma once

#include <cstddef>
#include <cstdint>

namespace npk {

// Low byte of Entry::flags.
enum class Compression : std::uint8_t {
    none = 0,
    zlib = 1,
    lz4 = 2,   // LZ4 block, no size prefix; output size is raw_size
    zstd = 3,
};

// One row of the table of contents (28 bytes on disk, little-endian u32s).
struct Entry {
    std::uint32_t id = 0;          // murmur3 of the path inside the archive
    std::uint32_t offset = 0;      // where the stored bytes start
    std::uint32_t packed_size = 0; // length of the stored bytes
    std::uint32_t raw_size = 0;    // length of the original file
    std::uint32_t crc_packed = 0;  // murmur3 of the stored bytes
    std::uint32_t crc_raw = 0;     // murmur3 of the original file
    std::uint32_t flags = 0;

    static constexpr std::size_t kDiskSize = 28;
    static constexpr std::uint32_t kEncryptedFlag = 0x10000;

    [[nodiscard]] constexpr Compression compression() const noexcept
    {
        return static_cast<Compression>(flags & 0xFFu);
    }
    [[nodiscard]] constexpr bool encrypted() const noexcept
    {
        return (flags & kEncryptedFlag) != 0;
    }
};

} // namespace npk
