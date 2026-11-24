#pragma once

#include <expected>
#include <string>

namespace npk {

enum class Errc {
    not_implemented,
    invalid_input,       // the user's input does not point at anything usable
    io_error,            // a file could not be opened, read or written
    bad_format,          // not an NXPK archive / not a resdb, or truncated
    crc_packed_mismatch, // stored bytes damaged
    decompress_failed,   // the codec rejected the data
    size_mismatch,       // decompressed size differs from raw_size
    crc_raw_mismatch,    // decoded file differs from the original
    unsupported,         // unknown compression mode, entry too large for a codec
    not_found,           // installation, archive or entry not found
};

struct Error {
    Errc code;
    std::string message;
};

template <class T>
using Result = std::expected<T, Error>;

[[nodiscard]] std::unexpected<Error> make_error(Errc code, std::string message);
[[nodiscard]] const char* to_string(Errc code) noexcept;

} // namespace npk
