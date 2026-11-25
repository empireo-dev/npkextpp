#include "npk/pipeline/disk_probe.hpp"

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
constexpr std::uint64_t kMinFileBytes = 16ull << 20;   // smaller files: too few places to seek between
constexpr auto kUntouchedFor = std::chrono::minutes(10);   // younger files may still sit in a cache
constexpr std::size_t kSearchEntries = 3000;              // looking for an existing file to read
constexpr auto kSearchTime = std::chrono::milliseconds(300);
constexpr std::size_t kMinReads = 30;
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

struct ReadTimes {
    double median_ms = 0;
    double slow_ms = 0;   // 90th percentile
    std::size_t reads = 0;
};

// Times random block reads from `path` (all zero if it could not be read).
// The places differ on every run, so an earlier probe has not left them in a cache.
ReadTimes time_reads(const fs::path& path, std::uint64_t size)
{
    UncachedFile file(path);
    if (!file.is_open() || size < 2 * kBlock) return {};
    auto block = std::make_unique<Block>();
    std::mt19937_64 random(std::random_device{}() ^ static_cast<std::uint64_t>(Clock::now().time_since_epoch().count()));
    std::uniform_int_distribution<std::uint64_t> pick(0, size / kBlock - 1);
    std::vector<double> times;
    const auto start = Clock::now();
    while (times.size() < kMaxReads) {
        const auto elapsed = Clock::now() - start;
        if (elapsed >= kMaxTime || (elapsed >= kMinTime && times.size() >= kMinReads)) break;
        const auto before = Clock::now();
        if (!file.read_at(pick(random) * kBlock, *block)) return {};
        times.push_back(std::chrono::duration<double, std::milli>(Clock::now() - before).count());
    }
    if (times.empty()) return {};
    std::ranges::sort(times);
    ReadTimes out;
    out.reads = times.size();
    out.median_ms = times[times.size() / 2];
    out.slow_ms = times[std::min(times.size() - 1, times.size() * 9 / 10)];
    return out;
}

// A file of the right size that was not written lately, so no cache holds it.
bool readable_candidate(const fs::path& path, std::uint64_t& size)
{
    std::error_code error;
    size = fs::file_size(path, error);
    if (error || size < kMinFileBytes) return false;
    const auto written = fs::last_write_time(path, error);
    return !error && fs::file_time_type::clock::now() - written >= kUntouchedFor;
}

// An existing file on the output's volume to read instead of a fresh test file:
// one in the output folder (from an earlier run), or near it. Gives up after a
// few thousand folder entries.
fs::path find_untouched_file(const fs::path& output_root, std::uint64_t& size)
{
    std::error_code error;
    const auto start = Clock::now();
    std::size_t entries = 0;
    fs::path folder = fs::absolute(output_root, error).lexically_normal();
    fs::path walked;   // the folder below `folder` that was already looked through
    while (!folder.empty()) {
        fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, error);
        for (; !error && it != fs::recursive_directory_iterator(); it.increment(error)) {
            if (++entries > kSearchEntries || Clock::now() - start > kSearchTime) return {};
            std::error_code type_error;
            if (it->is_directory(type_error)) {
                if (it->path() == walked || it->is_symlink(type_error) || !same_volume(it->path(), output_root)) {
                    it.disable_recursion_pending();
                }
            } else if (it->is_regular_file(type_error) && readable_candidate(it->path(), size)) {
                return it->path();
            }
        }
        error.clear();
        if (!folder.has_parent_path() || folder.parent_path() == folder || !same_volume(folder.parent_path(), output_root)) break;
        walked = folder;
        folder = folder.parent_path();
    }
    return {};
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

DiskKind classify(double slow_read_ms) noexcept
{
    if (!(slow_read_ms > 0)) return DiskKind::unknown;
    return slow_read_ms >= kHddReadMs ? DiskKind::hdd : DiskKind::ssd;
}

DiskProbe probe_disk(const fs::path& output_root, const std::vector<fs::path>& archives)
{
    DiskProbe probe;
    std::error_code error;

    // Writing: a test file in the output folder, flushed to the disk. Shown, not
    // used for the decision (caches make it vary too much); it is also read
    // below when nothing else can be.
    const fs::path test_file = output_root / kTestFileName;
    const auto write_started = Clock::now();
    const bool wrote = write_test_file(test_file, kTestFileBytes);
    const double write_seconds = std::chrono::duration<double>(Clock::now() - write_started).count();
    if (wrote && write_seconds > 0) probe.write_mb_per_s = static_cast<double>(kTestFileBytes) / write_seconds / 1e6;

    const auto measure = [&probe](const fs::path& path, std::uint64_t size, const char* what) {
        const ReadTimes times = time_reads(path, size);
        probe.median_read_ms = times.median_ms;
        probe.slow_read_ms = times.slow_ms;
        probe.reads = times.reads;
        probe.read_from = what;
        return times.reads > 0;
    };

    // 1. The largest archive, if it is on the output's volume and nobody wrote it lately.
    fs::path largest;
    std::uint64_t largest_size = 0;
    for (const fs::path& archive : archives) {
        const std::uint64_t size = fs::file_size(archive, error);
        if (!error && size > largest_size) largest = archive, largest_size = size;
    }
    bool measured = false;
    std::uint64_t size = 0;
    if (!largest.empty() && same_volume(largest, output_root) && readable_candidate(largest, size)) {
        measured = measure(largest, size, "an archive");
    }
    // 2. Some other file there that nobody wrote lately.
    if (!measured) {
        const fs::path other = find_untouched_file(output_root, size);
        if (!other.empty()) measured = measure(other, size, "an existing file");
    }
    // 3. The test file. Just written, so a disk's or a virtual machine's cache may
    //    still hold part of it; the 90th percentile still sees the rest.
    if (!measured) {
        if (wrote) measured = measure(test_file, kTestFileBytes, "the test file");
        else probe.problem = "cannot write a test file into the output folder";
    }
    fs::remove(test_file, error);
    probe.kind = classify(probe.slow_read_ms);
    if (probe.kind == DiskKind::unknown && probe.problem.empty()) probe.problem = "cannot read uncached";
    return probe;
}

} // namespace npk::pipeline
