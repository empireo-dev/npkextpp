#pragma once

#include "npk/error.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

namespace npk::pipeline {

// A file being written by a writer thread. Implemented per platform
// (output_file_posix.cpp / output_file_win32.cpp). Errors carry codes the
// writers act on: not_found (folder missing), too_many_open_files, and the
// fatal no_space / read_only.
class OutputFile {
public:
    OutputFile() = default;
    OutputFile(const OutputFile&) = delete;
    OutputFile& operator=(const OutputFile&) = delete;
    OutputFile(OutputFile&& other) noexcept;
    OutputFile& operator=(OutputFile&& other) noexcept;
    ~OutputFile();

    // Creates or truncates `path` (already in the OS's preferred form; on Windows
    // an extended-length path).
    [[nodiscard]] static Result<OutputFile> create(const std::filesystem::path& path);

    // Writes all of `data`, retrying short writes.
    [[nodiscard]] Result<void> write(std::span<const std::byte> data);

    // Linux: start writing the file's dirty pages to disk without waiting
    // (sync_file_range WRITE). No-op elsewhere.
    void start_writeback() noexcept;

    // Linux: wait until the file's data is on the disk, then drop its pages from
    // the cache (sync_file_range WAIT + posix_fadvise DONTNEED). No-op elsewhere.
    void finish_writeback() noexcept;

    [[nodiscard]] Result<void> close();
    [[nodiscard]] bool is_open() const noexcept { return handle_ != kInvalid; }

private:
    static constexpr std::intptr_t kInvalid = -1;   // fd on POSIX, HANDLE on Windows (INVALID_HANDLE_VALUE)
    explicit OutputFile(std::intptr_t handle) noexcept : handle_(handle) {}
    std::intptr_t handle_ = kInvalid;
};

// True for write errors after which nothing else can be written either.
[[nodiscard]] bool is_fatal(const Error& error) noexcept;

} // namespace npk::pipeline
