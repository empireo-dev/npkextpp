#pragma once

#include "npk/archive.hpp"
#include "npk/entry.hpp"
#include "npk/error.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace npk {

// Decompressor state for one thread (a zstd DCtx and a libdeflate
// decompressor). Create one per thread and reuse it for every entry; it is not
// safe to share one between threads.
class DecodeContext {
public:
    DecodeContext();
    ~DecodeContext();
    DecodeContext(DecodeContext&&) noexcept;
    DecodeContext& operator=(DecodeContext&&) noexcept;
    DecodeContext(const DecodeContext&) = delete;
    DecodeContext& operator=(const DecodeContext&) = delete;

private:
    friend Result<void> decode(std::span<std::byte>, const Entry&, DecodeContext&,
                               std::span<std::byte>, bool);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Number of leading bytes that are XOR-scrambled in encrypted entries.
inline constexpr std::size_t kEncryptedPrefix = 128;

// Undoes the XOR scrambling in place: byte i ^= (i + 6) & 0xFF, first 128 bytes.
void decrypt(std::span<std::byte> stored) noexcept;

// stored ──check crc_packed──► decrypt ──► decompress ──check crc_raw──► out
//
// `stored` is decrypted in place, so the caller's buffer is modified.
// `out` must be exactly entry.raw_size bytes.
[[nodiscard]] Result<void> decode(std::span<std::byte> stored, const Entry& entry,
                                  DecodeContext& context, std::span<std::byte> out,
                                  bool verify = true);

// One entry from start to end, on the calling thread: read, decode, return.
// This is the "extract a single record" path; whole archives go through the
// pipeline instead.
[[nodiscard]] Result<std::vector<std::byte>> read_entry(const Archive& archive, const Entry& entry,
                                                        DecodeContext& context, bool verify = true);

} // namespace npk
