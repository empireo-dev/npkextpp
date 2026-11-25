#include "disk_probe.hpp"

#include "npk/paths.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace npk::pipeline {

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kBlock = 4096;                      // one read; also the alignment uncached reads need
constexpr std::uint64_t kTestFileBytes = 16ull << 20;
constexpr std::uint64_t kMinArchiveBytes = 64ull << 20;   // smaller archives: write a test file instead
constexpr std::size_t kMinReads = 16;
constexpr std::size_t kMaxReads = 128;
constexpr auto kMinTime = std::chrono::milliseconds(300);
constexpr auto kMaxTime = std::chrono::seconds(2);
constexpr const char* kTestFileName = ".npkextpp-disk-test.tmp";

struct alignas(kBlock) Block {
    std::byte data[kBlock];
};

#if defined(_WIN32)

class UncachedFile {
public:
    explicit UncachedFile(const fs::path& path)
        : handle_(::CreateFileW(long_path(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING | FILE_FLAG_RANDOM_ACCESS, nullptr))
    {
    }
    ~UncachedFile()
    {
        if (handle_ != INVALID_HANDLE_VALUE) ::CloseHandle(handle_);
    }
    UncachedFile(const UncachedFile&) = delete;
    UncachedFile& operator=(const UncachedFile&) = delete;

    [[nodiscard]] bool is_open() const noexcept { return handle_ != INVALID_HANDLE_VALUE; }
    bool read_at(std::uint64_t offset, Block& block)
    {
        OVERLAPPED at{};
        at.Offset = static_cast<DWORD>(offset);
        at.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD got = 0;
        return ::ReadFile(handle_, block.data, kBlock, &got, &at) && got == kBlock;
    }

private:
    HANDLE handle_;
};

bool write_test_file(const fs::path& path, std::uint64_t bytes)
{
    const HANDLE file = ::CreateFileW(long_path(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    std::vector<std::uint64_t> chunk((1u << 20) / sizeof(std::uint64_t));
    std::mt19937_64 random(7);   // not zeros: some disks and file systems skip or compress those
    bool ok = true;
    for (std::uint64_t written = 0; ok && written < bytes; written += 1u << 20) {
        for (auto& word : chunk) word = random();
        DWORD done = 0;
        ok = ::WriteFile(file, chunk.data(), 1u << 20, &done, nullptr) && done == (1u << 20);
    }
    ok = ok && ::FlushFileBuffers(file);
    ::CloseHandle(file);
    return ok;
}

bool same_volume(const fs::path& a, const fs::path& b)
{
    wchar_t va[MAX_PATH + 1]{}, vb[MAX_PATH + 1]{};
    if (!::GetVolumePathNameW(a.c_str(), va, MAX_PATH) || !::GetVolumePathNameW(b.c_str(), vb, MAX_PATH)) return false;
    return ::CompareStringOrdinal(va, -1, vb, -1, TRUE) == CSTR_EQUAL;
}

#else

class UncachedFile {
public:
    explicit UncachedFile(const fs::path& path)
    {
#ifdef O_DIRECT
        fd_ = ::open(path.c_str(), O_RDONLY | O_DIRECT);
#endif
        if (fd_ < 0) {   // no O_DIRECT here (tmpfs, say): drop the file from the cache instead
            fd_ = ::open(path.c_str(), O_RDONLY);
#ifdef POSIX_FADV_DONTNEED
            if (fd_ >= 0) (void)::posix_fadvise(fd_, 0, 0, POSIX_FADV_DONTNEED);
#endif
        }
#ifdef F_NOCACHE
        if (fd_ >= 0) (void)::fcntl(fd_, F_NOCACHE, 1);
#endif
    }
    ~UncachedFile()
    {
        if (fd_ >= 0) ::close(fd_);
    }
    UncachedFile(const UncachedFile&) = delete;
    UncachedFile& operator=(const UncachedFile&) = delete;

    [[nodiscard]] bool is_open() const noexcept { return fd_ >= 0; }
    bool read_at(std::uint64_t offset, Block& block)
    {
        return ::pread(fd_, block.data, kBlock, static_cast<off_t>(offset)) == static_cast<ssize_t>(kBlock);
    }

private:
    int fd_ = -1;
};

bool write_test_file(const fs::path& path, std::uint64_t bytes)
{
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;
    std::vector<std::uint64_t> chunk((1u << 20) / sizeof(std::uint64_t));
    std::mt19937_64 random(7);   // not zeros: some disks and file systems skip or compress those
    bool ok = true;
    for (std::uint64_t written = 0; ok && written < bytes; written += 1u << 20) {
        for (auto& word : chunk) word = random();
        ok = ::write(fd, chunk.data(), 1u << 20) == (1 << 20);
    }
    ok = ok && ::fsync(fd) == 0;
    ::close(fd);
    return ok;
}

bool same_volume(const fs::path& a, const fs::path& b)
{
    struct stat sa{}, sb{};
    return ::stat(a.c_str(), &sa) == 0 && ::stat(b.c_str(), &sb) == 0 && sa.st_dev == sb.st_dev;
}

#endif

// Median time of random block reads from `path` (0 if it could not be read).
double median_read_ms(const fs::path& path, std::uint64_t size, std::size_t& reads)
{
    UncachedFile file(path);
    if (!file.is_open() || size < 2 * kBlock) return 0;
    auto block = std::make_unique<Block>();
    std::mt19937_64 random(1);
    std::uniform_int_distribution<std::uint64_t> pick(0, size / kBlock - 1);
    std::vector<double> times;
    const auto start = Clock::now();
    while (times.size() < kMaxReads) {
        const auto elapsed = Clock::now() - start;
        if (elapsed >= kMaxTime || (elapsed >= kMinTime && times.size() >= kMinReads)) break;
        const auto before = Clock::now();
        if (!file.read_at(pick(random) * kBlock, *block)) return 0;
        times.push_back(std::chrono::duration<double, std::milli>(Clock::now() - before).count());
    }
    reads = times.size();
    if (times.empty()) return 0;
    const auto middle = times.begin() + static_cast<std::ptrdiff_t>(times.size() / 2);
    std::nth_element(times.begin(), middle, times.end());
    return *middle;
}

} // namespace

const char* name_of(DiskKind kind) noexcept
{
    switch (kind) {
    case DiskKind::ssd: return "SSD";
    case DiskKind::hdd: return "hard disk";
    case DiskKind::unknown: break;
    }
    return "unknown disk";
}

DiskKind classify(double median_read_ms) noexcept
{
    if (!(median_read_ms > 0)) return DiskKind::unknown;
    return median_read_ms >= kHddReadMs ? DiskKind::hdd : DiskKind::ssd;
}

DiskProbe probe_disk(const fs::path& output_root, const std::vector<fs::path>& archives)
{
    DiskProbe probe;
    std::error_code error;

    // The largest archive on the output's volume, if one is big enough.
    fs::path largest;
    std::uint64_t largest_size = 0;
    for (const fs::path& archive : archives) {
        const std::uint64_t size = fs::file_size(archive, error);
        if (!error && size > largest_size) largest = archive, largest_size = size;
    }
    if (largest_size >= kMinArchiveBytes && same_volume(largest, output_root)) {
        probe.median_read_ms = median_read_ms(largest, largest_size, probe.reads);
        probe.read_from = "an archive";
    }
    if (!(probe.median_read_ms > 0)) {
        const fs::path test_file = output_root / kTestFileName;
        if (write_test_file(test_file, kTestFileBytes)) {
            probe.median_read_ms = median_read_ms(test_file, kTestFileBytes, probe.reads);
            probe.read_from = "a test file";
        } else {
            probe.problem = "cannot write a test file into the output folder";
        }
        fs::remove(test_file, error);
    }
    probe.kind = classify(probe.median_read_ms);
    if (probe.kind == DiskKind::unknown && probe.problem.empty()) probe.problem = "cannot read uncached";
    return probe;
}

} // namespace npk::pipeline
