#pragma once

#include "npk/entry.hpp"
#include "npk/error.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace npk {

// The stored bytes of a file as an entry with these flags holds it: compressed
// with the entry's method (none, zlib, LZ4 block, zstd), then its first bytes
// XOR-scrambled if the entry is encrypted. The reverse of Decoder::decode().
// The compressed bytes are not those of the game's own packer (another
// compressor build or level), but decode to the same file.
[[nodiscard]] Result<std::vector<std::byte>> encode(std::span<const std::byte> content, std::uint32_t flags);

// A table-of-contents row for `content` stored as `stored` at `offset`.
[[nodiscard]] Entry make_entry(std::uint32_t id, std::uint32_t offset, std::span<const std::byte> content,
                               std::span<const std::byte> stored, std::uint32_t flags) noexcept;

} // namespace npk
