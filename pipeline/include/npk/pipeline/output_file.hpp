#pragma once

#include "npk/error.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

namespace npk::pipeline {

// A file being written by a writer thread. Implemented per platform
// (output_file_posix.cpp / output_file_win32.cpp).
class OutputFile {
public:
    OutputFile() = default;
    OutputFile(const OutputFile&) = delete;
    OutputFile& operator=(const OutputFile&) = delete;
    OutputFile(OutputFile&& other) noexcept;
    OutputFile& operator=(OutputFile&& other) noexcept;
    ~OutputFile();

    // Creates or truncates. `size` lets the filesystem allocate it in one piece.
    [[nodiscard]] static Result<OutputFile> create(const std::filesystem::path& path, std::uint64_t size);

    [[nodiscard]] Result<void> write(std::span<const std::byte> data);

    // Linux: sync_file_range(SYNC_FILE_RANGE_WRITE) – start writeback, don't wait.
    void start_writeback() noexcept;

    [[nodiscard]] Result<void> close();

    [[nodiscard]] std::intptr_t native() const noexcept { return handle_; }

private:
    std::intptr_t handle_ = -1;   // fd on POSIX, HANDLE on Windows
};

} // namespace npk::pipeline
