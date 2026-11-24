#pragma once

#include <cstddef>
#include <span>
#include <string_view>

namespace npk {

inline constexpr std::string_view kUnknownExtension = "bin";

// A file extension (without the dot) that fits the file's first bytes. Only used
// for entries whose name could not be recovered from the manifest.
[[nodiscard]] std::string_view guess_extension(std::span<const std::byte> data) noexcept;

} // namespace npk
