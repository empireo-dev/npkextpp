#include "npk/input_file.hpp"

#include "npk/paths.hpp"

#include <algorithm>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace npk {

namespace {

// Largest single read; keeps the size within what every OS call accepts.
constexpr std::size_t kMaxSingleRead = std::size_t{1} << 30;

std::string os_error_text(int code)
{
    return std::system_category().message(code);
}

#if defined(_WIN32)
int last_error() { return static_cast<int>(::GetLastError()); }
#else
int last_error() { return errno; }
#endif

} // namespace

InputFile::InputFile(InputFile&& other) noexcept
#if defined(_WIN32)
    : handle_(std::exchange(other.handle_, nullptr))
#else
    : fd_(std::exchange(other.fd_, -1))
#endif
    , size_(std::exchange(other.size_, 0))
{
}

InputFile& InputFile::operator=(InputFile&& other) noexcept
{
    if (this != &other) {
        close();
#if defined(_WIN32)
        handle_ = std::exchange(other.handle_, nullptr);
#else
        fd_ = std::exchange(other.fd_, -1);
#endif
        size_ = std::exchange(other.size_, 0);
    }
    return *this;
}

InputFile::~InputFile() { close(); }

bool InputFile::is_open() const noexcept
{
#if defined(_WIN32)
    return handle_ != nullptr;
#else
    return fd_ >= 0;
#endif
}

Result<InputFile> InputFile::open(const std::filesystem::path& path)
{
    InputFile file;
#if defined(_WIN32)
    HANDLE handle = ::CreateFileW(long_path(path).c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return make_error(Errc::io_error, "cannot open " + to_utf8(path) + ": " + os_error_text(last_error()));
    }
    file.handle_ = handle;
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(handle, &size)) {
        return make_error(Errc::io_error, "cannot get the size of " + to_utf8(path) + ": " +
                                              os_error_text(last_error()));
    }
    file.size_ = static_cast<std::uint64_t>(size.QuadPart);
#else
    int fd;
    do {
        fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        return make_error(Errc::io_error, "cannot open " + to_utf8(path) + ": " + os_error_text(last_error()));
    }
    file.fd_ = fd;
    struct stat info{};
    if (::fstat(fd, &info) != 0) {
        return make_error(Errc::io_error, "cannot get the size of " + to_utf8(path) + ": " +
                                              os_error_text(last_error()));
    }
    if (!S_ISREG(info.st_mode)) {
        return make_error(Errc::io_error, to_utf8(path) + " is not a regular file");
    }
    file.size_ = static_cast<std::uint64_t>(info.st_size);
#endif
    return file;
}

Result<std::vector<std::byte>> InputFile::read_all(const std::filesystem::path& path)
{
    auto file = open(path);
    if (!file) return std::unexpected(std::move(file.error()));
    std::vector<std::byte> data(static_cast<std::size_t>(file->size()));
    if (auto read = file->read_at(0, data); !read) return std::unexpected(std::move(read.error()));
    return data;
}

Result<void> InputFile::read_at(std::uint64_t offset, std::span<std::byte> out) const
{
    if (!is_open()) return make_error(Errc::io_error, "read from a closed file");
    if (offset > size_ || out.size() > size_ - offset) {
        return make_error(Errc::bad_format, "read of " + std::to_string(out.size()) + " bytes at offset " +
                                                std::to_string(offset) + " runs past the end of the file");
    }

    std::size_t done = 0;
    while (done < out.size()) {
        const std::size_t want = std::min(out.size() - done, kMaxSingleRead);
#if defined(_WIN32)
        const std::uint64_t at = offset + done;
        OVERLAPPED position{};
        position.Offset = static_cast<DWORD>(at & 0xFFFFFFFFu);
        position.OffsetHigh = static_cast<DWORD>(at >> 32);
        DWORD got = 0;
        if (!::ReadFile(static_cast<HANDLE>(handle_), out.data() + done, static_cast<DWORD>(want), &got,
                        &position)) {
            return make_error(Errc::io_error, "read failed: " + os_error_text(last_error()));
        }
#else
        const ssize_t got = ::pread(fd_, out.data() + done, want, static_cast<off_t>(offset + done));
        if (got < 0) {
            if (errno == EINTR) continue;
            return make_error(Errc::io_error, "read failed: " + os_error_text(last_error()));
        }
#endif
        if (got == 0) return make_error(Errc::io_error, "unexpected end of file");
        done += static_cast<std::size_t>(got);
    }
    return {};
}

void InputFile::advise_sequential() const noexcept
{
#if defined(POSIX_FADV_SEQUENTIAL)
    if (fd_ >= 0) (void)::posix_fadvise(fd_, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
}

void InputFile::forget(std::uint64_t offset, std::uint64_t length) const noexcept
{
#if defined(POSIX_FADV_DONTNEED)
    if (fd_ >= 0) {
        (void)::posix_fadvise(fd_, static_cast<off_t>(offset), static_cast<off_t>(length), POSIX_FADV_DONTNEED);
    }
#else
    (void)offset;
    (void)length;
#endif
}

void InputFile::close() noexcept
{
#if defined(_WIN32)
    if (handle_ != nullptr) ::CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
#else
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
#endif
    size_ = 0;
}

} // namespace npk
