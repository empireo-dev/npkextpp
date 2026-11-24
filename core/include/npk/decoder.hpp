#pragma once

#include "npk/archive.hpp"
#include "npk/entry.hpp"
#include "npk/error.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace npk {

// Number of leading bytes that are XOR-scrambled in encrypted entries.
inline constexpr std::size_t kEncryptedPrefix = 128;

// The XOR scrambling of encrypted entries, in place: byte i ^= (i + 6) & 0xFF for
// the first 128 bytes. It is its own inverse, so this also encrypts.
void decrypt(std::span<std::byte> stored) noexcept;

// Turns stored entries back into the original files. Holds everything that is
// worth keeping between entries: a zstd decompression context, a libdeflate
// decompressor and a read buffer. Create one per thread and reuse it for every
// entry; one Decoder must not be used by two threads at the same time.
//
//   stored ──check crc_packed──► decrypt ──► decompress ──check crc_raw──► original
class Decoder {
public:
    Decoder();
    ~Decoder();
    Decoder(Decoder&&) noexcept;
    Decoder& operator=(Decoder&&) noexcept;
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    // Decodes one entry from its stored bytes into `out`, which must be exactly
    // entry.raw_size bytes. `stored` is decrypted in place (the caller's buffer
    // is modified). With `verify`, both checksums are checked, so a successful
    // result is byte-identical to the file that was packed.
    [[nodiscard]] Result<void> decode(std::span<std::byte> stored, const Entry& entry,
                                      std::span<std::byte> out, bool verify = true);

    // Decompresses already decrypted data with the entry's compression mode into
    // `out` (exactly raw_size bytes). No checksums.
    [[nodiscard]] Result<void> decompress(std::span<const std::byte> data, const Entry& entry,
                                          std::span<std::byte> out);

    // One entry from start to end on the calling thread: read, decode, return.
    // The stored bytes go through an internal buffer that is reused between calls.
    [[nodiscard]] Result<std::vector<std::byte>> read(const Archive& archive, const Entry& entry,
                                                      bool verify = true);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace npk
