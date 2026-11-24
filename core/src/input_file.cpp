#include "npk/input_file.hpp"

#include <utility>

namespace npk {

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
    (void)path;
    return make_error(Errc::not_implemented, "InputFile::open");
}

Result<void> InputFile::read_at(std::uint64_t offset, std::span<std::byte> out) const
{
    (void)offset;
    (void)out;
    return make_error(Errc::not_implemented, "InputFile::read_at");
}

void InputFile::forget(std::uint64_t offset, std::uint64_t length) const noexcept
{
    (void)offset;
    (void)length;
}

void InputFile::close() noexcept
{
    // TODO: close the handle / descriptor
#if defined(_WIN32)
    handle_ = nullptr;
#else
    fd_ = -1;
#endif
    size_ = 0;
}

} // namespace npk
