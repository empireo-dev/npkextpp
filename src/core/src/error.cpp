#include "npk/error.hpp"

#include <utility>

namespace npk {

std::unexpected<Error> make_error(Errc code, std::string message)
{
    return std::unexpected(Error{code, std::move(message)});
}

const char* to_string(Errc code) noexcept
{
    switch (code) {
    case Errc::not_implemented: return "not implemented";
    case Errc::invalid_input: return "invalid input";
    case Errc::io_error: return "I/O error";
    case Errc::bad_format: return "bad format";
    case Errc::crc_packed_mismatch: return "stored data checksum mismatch";
    case Errc::decompress_failed: return "decompression failed";
    case Errc::size_mismatch: return "size mismatch";
    case Errc::crc_raw_mismatch: return "decoded data checksum mismatch";
    case Errc::unsupported: return "unsupported";
    case Errc::not_found: return "not found";
    case Errc::no_space: return "no space left on the disk";
    case Errc::read_only: return "read-only filesystem";
    case Errc::too_many_open_files: return "too many open files";
    }
    return "unknown error";
}

} // namespace npk
