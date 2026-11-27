#include "npk/pipeline/output_file.hpp"

#include "npk/paths.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <system_error>
#include <utility>

namespace npk::pipeline {

namespace {

Error os_error(DWORD code, const std::string& what)
{
    Errc errc = Errc::io_error;
    switch (code) {
    case ERROR_PATH_NOT_FOUND:
    case ERROR_FILE_NOT_FOUND: errc = Errc::not_found; break;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
    case ERROR_DISK_QUOTA_EXCEEDED: errc = Errc::no_space; break;
    case ERROR_WRITE_PROTECT: errc = Errc::read_only; break;
    case ERROR_TOO_MANY_OPEN_FILES: errc = Errc::too_many_open_files; break;
    default: break;
    }
    return {errc, what + ": " + std::system_category().message(static_cast<int>(code))};
}

HANDLE handle_of(std::intptr_t handle) { return reinterpret_cast<HANDLE>(handle); }

constexpr DWORD kMaxSingleWrite = DWORD{1} << 30;

} // namespace

bool is_fatal(const Error& error) noexcept
{
    return error.code == Errc::no_space || error.code == Errc::read_only;
}

OutputFile::OutputFile(OutputFile&& other) noexcept : handle_(std::exchange(other.handle_, kInvalid)) {}

OutputFile& OutputFile::operator=(OutputFile&& other) noexcept
{
    if (this != &other) {
        (void)close();
        handle_ = std::exchange(other.handle_, kInvalid);
    }
    return *this;
}

OutputFile::~OutputFile() { (void)close(); }

Result<OutputFile> OutputFile::create(const std::filesystem::path& path)
{
    HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return std::unexpected(os_error(::GetLastError(), "cannot create " + to_utf8(path)));
    return OutputFile(reinterpret_cast<std::intptr_t>(handle));
}

Result<void> OutputFile::write(std::span<const std::byte> data)
{
    std::size_t done = 0;
    while (done < data.size()) {
        const DWORD want = static_cast<DWORD>(std::min<std::size_t>(data.size() - done, kMaxSingleWrite));
        DWORD wrote = 0;
        if (!::WriteFile(handle_of(handle_), data.data() + done, want, &wrote, nullptr)) {
            return std::unexpected(os_error(::GetLastError(), "write failed"));
        }
        if (wrote == 0) return std::unexpected(Error{Errc::io_error, "write failed: nothing written"});
        done += wrote;
    }
    return {};
}

void OutputFile::start_writeback() noexcept {}

void OutputFile::finish_writeback() noexcept {}

Result<void> OutputFile::close()
{
    if (!is_open()) return {};
    HANDLE handle = handle_of(std::exchange(handle_, kInvalid));
    if (!::CloseHandle(handle)) return std::unexpected(os_error(::GetLastError(), "close failed"));
    return {};
}

} // namespace npk::pipeline
