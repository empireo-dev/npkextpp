#pragma once

// Little-endian loads/stores used by the format code (private to npk_core).

#include <bit>
#include <cstdint>
#include <cstring>

namespace npk::detail {

[[nodiscard]] inline std::uint32_t load_le32(const void* p) noexcept
{
    std::uint32_t value;
    std::memcpy(&value, p, sizeof value);
    if constexpr (std::endian::native == std::endian::big) value = std::byteswap(value);
    return value;
}

inline void store_le32(void* p, std::uint32_t value) noexcept
{
    if constexpr (std::endian::native == std::endian::big) value = std::byteswap(value);
    std::memcpy(p, &value, sizeof value);
}

} // namespace npk::detail
