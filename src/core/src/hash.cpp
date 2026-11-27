#include "npk/hash.hpp"

#include "bytes.hpp"

#include <bit>

namespace npk {

// MurmurHash3_x86_32 (Austin Appleby, public domain), byte-for-byte the
// reference algorithm: 4-byte little-endian blocks, tail, length, fmix32.
std::uint32_t murmur3_32(std::span<const std::byte> data, std::uint32_t seed) noexcept
{
    constexpr std::uint32_t c1 = 0xcc9e2d51u;
    constexpr std::uint32_t c2 = 0x1b873593u;

    const auto* bytes = reinterpret_cast<const unsigned char*>(data.data());
    const std::size_t length = data.size();
    const std::size_t blocks = length / 4;

    std::uint32_t h = seed;
    for (std::size_t i = 0; i < blocks; ++i) {
        std::uint32_t k = detail::load_le32(bytes + i * 4);
        k *= c1;
        k = std::rotl(k, 15);
        k *= c2;
        h ^= k;
        h = std::rotl(h, 13);
        h = h * 5 + 0xe6546b64u;
    }

    const unsigned char* tail = bytes + blocks * 4;
    std::uint32_t k = 0;
    switch (length & 3) {
    case 3: k ^= std::uint32_t{tail[2]} << 16; [[fallthrough]];
    case 2: k ^= std::uint32_t{tail[1]} << 8; [[fallthrough]];
    case 1:
        k ^= tail[0];
        k *= c1;
        k = std::rotl(k, 15);
        k *= c2;
        h ^= k;
    }

    h ^= static_cast<std::uint32_t>(length);
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

std::uint32_t murmur3_32(std::string_view text, std::uint32_t seed) noexcept
{
    return murmur3_32(std::as_bytes(std::span(text.data(), text.size())), seed);
}

} // namespace npk
