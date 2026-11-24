#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace npk {

// NeoX uses 32-bit murmur3 with this seed for entry ids (hash of the path) and
// for both checksums in the table of contents.
inline constexpr std::uint32_t kMurmurSeed = 0x9747B28Cu;

[[nodiscard]] std::uint32_t murmur3_32(std::span<const std::byte> data,
                                       std::uint32_t seed = kMurmurSeed) noexcept;
[[nodiscard]] std::uint32_t murmur3_32(std::string_view text,
                                       std::uint32_t seed = kMurmurSeed) noexcept;

} // namespace npk
