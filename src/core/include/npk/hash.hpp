#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace npk {

// NeoX uses 32-bit murmur3 (x86 variant) with this seed for entry ids (hash of
// the path inside the archive) and for both checksums in the table of contents.
inline constexpr std::uint32_t kMurmurSeed = 0x9747B28Cu;

[[nodiscard]] std::uint32_t murmur3_32(std::span<const std::byte> data,
                                       std::uint32_t seed = kMurmurSeed) noexcept;
[[nodiscard]] std::uint32_t murmur3_32(std::string_view text,
                                       std::uint32_t seed = kMurmurSeed) noexcept;

// The entry id of a path inside an archive: UTF-8, backslash separators,
// original case ("textures\\hero.dds").
[[nodiscard]] inline std::uint32_t path_id(std::string_view inner_path) noexcept
{
    return murmur3_32(inner_path);
}

} // namespace npk
