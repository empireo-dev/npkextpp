#include "npk/pipeline/output_file.hpp"

#include "npk/paths.hpp"

#include <cerrno>
#include <fcntl.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace npk::pipeline {

namespace {

Error os_error(int code, const std::string& what)
{
    Errc errc = Errc::io_error;
    switch (code) {
    case ENOENT: errc = Errc::not_found; break;
    case ENOSPC:
#if defined(EDQUOT)
    case EDQUOT:
#endif
        errc = Errc::no_space;
        break;
    case EROFS: errc = Errc::read_only; break;
    case EMFILE:
    case ENFILE: errc = Errc::too_many_open_files; break;
    default: break;
    }
    return {errc, what + ": " + std::system_category().message(code)};
}

int fd_of(std::intptr_t handle) { return static_cast<int>(handle); }

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
    int fd;
    do {
        fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) return std::unexpected(os_error(errno, "cannot create " + to_utf8(path)));
    return OutputFile(fd);
}

Result<void> OutputFile::write(std::span<const std::byte> data)
{
    std::size_t done = 0;
    while (done < data.size()) {
        const ssize_t n = ::write(fd_of(handle_), data.data() + done, data.size() - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            return std::unexpected(os_error(errno, "write failed"));
        }
        done += static_cast<std::size_t>(n);
    }
    return {};
}

void OutputFile::start_writeback() noexcept
{
#if defined(__linux__)
    if (is_open()) (void)::sync_file_range(fd_of(handle_), 0, 0, SYNC_FILE_RANGE_WRITE);
#endif
}

void OutputFile::finish_writeback() noexcept
{
#if defined(__linux__)
    if (!is_open()) return;
    (void)::sync_file_range(fd_of(handle_), 0, 0,
                            SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE | SYNC_FILE_RANGE_WAIT_AFTER);
    (void)::posix_fadvise(fd_of(handle_), 0, 0, POSIX_FADV_DONTNEED);
#endif
}

Result<void> OutputFile::close()
{
    if (!is_open()) return {};
    const int fd = fd_of(std::exchange(handle_, kInvalid));
    if (::close(fd) != 0 && errno != EINTR) return std::unexpected(os_error(errno, "close failed"));
    return {};
}

} // namespace npk::pipeline
