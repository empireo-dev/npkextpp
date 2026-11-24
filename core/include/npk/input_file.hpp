#pragma once

#include "npk/error.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace npk {

// A read-only file with positional reads (pread / ReadFile with an offset).
// read_at() does not move a shared file position, so one InputFile can be read
// from several threads at once.
class InputFile {
public:
    InputFile() = default;
    InputFile(const InputFile&) = delete;
    InputFile& operator=(const InputFile&) = delete;
    InputFile(InputFile&& other) noexcept;
    InputFile& operator=(InputFile&& other) noexcept;
    ~InputFile();

    [[nodiscard]] static Result<InputFile> open(const std::filesystem::path& path);

    // Whole file in one go (small files: resdb).
    [[nodiscard]] static Result<std::vector<std::byte>> read_all(const std::filesystem::path& path);

    [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
    [[nodiscard]] bool is_open() const noexcept;

    // Reads exactly out.size() bytes starting at offset.
    [[nodiscard]] Result<void> read_at(std::uint64_t offset, std::span<std::byte> out) const;

    // Hint that the file will be read front to back (bigger read-ahead).
    void advise_sequential() const noexcept;

    // Hint that the range will not be read again, so its cached pages can go
    // (posix_fadvise DONTNEED). Does nothing where unsupported.
    void forget(std::uint64_t offset, std::uint64_t length) const noexcept;

    void close() noexcept;

private:
#if defined(_WIN32)
    void* handle_ = nullptr;
#else
    int fd_ = -1;
#endif
    std::uint64_t size_ = 0;
};

} // namespace npk
