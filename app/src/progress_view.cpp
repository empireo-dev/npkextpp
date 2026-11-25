#include "progress_view.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <io.h>
#define NPK_ISATTY(fd) _isatty(fd)
#define NPK_FILENO(f) _fileno(f)
#else
#include <unistd.h>
#define NPK_ISATTY(fd) ::isatty(fd)
#define NPK_FILENO(f) ::fileno(f)
#endif

namespace npkextpp {

std::string format_bytes(std::uint64_t bytes)
{
    const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024 && unit < 4) {
        value /= 1024;
        ++unit;
    }
    char text[32];
    std::snprintf(text, sizeof text, unit == 0 ? "%.0f %s" : "%.1f %s", value, units[unit]);
    return text;
}

std::string format_count(std::uint64_t count)
{
    std::string digits = std::to_string(count);
    std::string out;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i != 0 && (digits.size() - i) % 3 == 0) out += ',';
        out += digits[i];
    }
    return out;
}

std::string format_duration(double seconds)
{
    char text[32];
    if (seconds < 60) {
        std::snprintf(text, sizeof text, "%.1f s", seconds);
    } else {
        const auto total = static_cast<long long>(seconds + 0.5);
        if (total >= 3600) std::snprintf(text, sizeof text, "%lld h %02lld min", total / 3600, (total % 3600) / 60);
        else std::snprintf(text, sizeof text, "%lld min %02lld s", total / 60, total % 60);
    }
    return text;
}

Console::Console() : interactive_(NPK_ISATTY(NPK_FILENO(stderr)) != 0) {}

void Console::clear_progress_locked()
{
    if (drawn_ == 0) return;
    std::fprintf(stderr, "\r%*s\r", static_cast<int>(drawn_), "");
    std::fflush(stderr);
    drawn_ = 0;
}

void Console::draw_locked(const std::string& text)
{
    const std::size_t pad = drawn_ > text.size() ? drawn_ - text.size() : 0;
    std::fprintf(stderr, "\r%s%*s", text.c_str(), static_cast<int>(pad), "");
    std::fflush(stderr);
    drawn_ = text.size();
}

void Console::line(std::string_view text)
{
    std::lock_guard lock(mutex_);
    clear_progress_locked();
    std::fprintf(stdout, "%.*s\n", static_cast<int>(text.size()), text.data());
    std::fflush(stdout);
    if (!last_progress_.empty() && interactive_) draw_locked(last_progress_);
}

void Console::error(std::string_view text)
{
    std::lock_guard lock(mutex_);
    clear_progress_locked();
    std::fprintf(stderr, "error: %.*s\n", static_cast<int>(text.size()), text.data());
    std::fflush(stderr);
}

void Console::show_progress(const npk::pipeline::Progress& progress, std::uint64_t total_files,
                            std::uint64_t total_bytes, std::stop_token stop, const std::function<void()>& on_tick)
{
    using clock = std::chrono::steady_clock;
    const auto started = clock::now();
    auto last_at = started;
    std::uint64_t last_bytes = 0;
    double rate = 0;

    std::mutex sleep_mutex;
    std::condition_variable_any sleeper;
    while (!stop.stop_requested()) {
        {
            std::unique_lock lock(sleep_mutex);
            sleeper.wait_for(lock, stop, std::chrono::milliseconds(250), [] { return false; });
        }
        if (on_tick) on_tick();
        if (!interactive_ || stop.stop_requested()) continue;

        const auto now = clock::now();
        const std::uint64_t files = progress.files_written.load() + progress.files_failed.load();
        const std::uint64_t bytes = progress.bytes_written.load();
        const double dt = std::chrono::duration<double>(now - last_at).count();
        if (dt >= 1.0) {
            const double current = static_cast<double>(bytes - last_bytes) / dt;
            rate = rate == 0 ? current : rate * 0.6 + current * 0.4;
            last_at = now;
            last_bytes = bytes;
        }
        const double share = total_files ? static_cast<double>(files) / static_cast<double>(total_files) : 0;

        std::string text;
        char head[64];
        std::snprintf(head, sizeof head, "%5.1f%%  ", share * 100);
        text += head;
        text += format_count(files) + " / " + format_count(total_files) + " files  ";
        text += format_bytes(bytes) + " / " + format_bytes(total_bytes) + "  ";
        text += format_bytes(static_cast<std::uint64_t>(rate)) + "/s  ";
        text += std::to_string(progress.writers_active.load()) + " writers";
        if (const npk::ArchiveSource* source = progress.current_source.load()) {
            text += "  " + source->label();
        }
        if (text.size() > 150) text.resize(150);

        std::lock_guard lock(mutex_);
        last_progress_ = text;
        draw_locked(text);
    }
    std::lock_guard lock(mutex_);
    clear_progress_locked();
    last_progress_.clear();
}

} // namespace npkextpp
