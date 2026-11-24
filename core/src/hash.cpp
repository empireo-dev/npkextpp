#include "npk/hash.hpp"

namespace npk {

std::uint32_t murmur3_32(std::span<const std::byte> data, std::uint32_t seed) noexcept
{
    (void)data;
    return seed; // TODO: murmur3 x86_32
}

std::uint32_t murmur3_32(std::string_view text, std::uint32_t seed) noexcept
{
    return murmur3_32(std::as_bytes(std::span(text.data(), text.size())), seed);
}

} // namespace npk
