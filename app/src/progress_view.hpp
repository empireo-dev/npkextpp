#pragma once

#include "npk/pipeline/progress.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>

namespace npkextpp {

// The terminal: a one-line progress display redrawn a few times per second
// (only when stderr is a terminal), and log lines printed above it. Thread-safe.
class Console {
public:
    Console();

    void line(std::string_view text);   // a permanent line (stdout), above the progress line
    void error(std::string_view text);  // "error: ..." on stderr

    // Redraws progress until stop is requested. Every tick also calls `on_tick`
    // (used to turn Ctrl+C into a pipeline stop).
    void show_progress(const npk::pipeline::Progress& progress, std::uint64_t total_files, std::uint64_t total_bytes,
                       std::stop_token stop, const std::function<void()>& on_tick);

    [[nodiscard]] bool interactive() const noexcept { return interactive_; }

private:
    void clear_progress_locked();
    void draw_locked(const std::string& text);

    std::mutex mutex_;
    bool interactive_ = false;
    std::size_t drawn_ = 0;   // length of the progress line on screen
    std::string last_progress_;
};

[[nodiscard]] std::string format_bytes(std::uint64_t bytes);
[[nodiscard]] std::string format_count(std::uint64_t count);   // 1,234,567
[[nodiscard]] std::string format_duration(double seconds);

} // namespace npkextpp
