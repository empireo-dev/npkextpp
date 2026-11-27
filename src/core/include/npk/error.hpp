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
    not_found,           // installation, archive, entry or folder not found
    no_space,            // disk full or quota exceeded
    read_only,           // the output is on a read-only filesystem / write-protected
    too_many_open_files, // the process ran out of file handles
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
