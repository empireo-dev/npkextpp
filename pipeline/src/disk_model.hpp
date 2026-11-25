#pragma once

// What the output disk costs, learned while the pipeline runs (private to
// npk_pipeline). See docs/pipeline.md §7.
//
// While writing is the limit, a window's time is spent on what was written in it:
//
//     seconds = a[W] * files + b * bytes written + c * bytes read
//
// a[W] is the cost of one file at W writers: what concurrency changes (an SSD
// hides it, a hard disk pays seeks for it). b and c are shared by all W. Writer
// counts are compared by their predicted time for the current mix of files and
// bytes, so a stretch of tiny files and a stretch of textures don't look like a
// better or worse writer count.

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

namespace npk::pipeline {

// What happened in one window.
struct Window {
    double seconds = 0;
    double files = 0;
    double bytes_written = 0;
    double bytes_read = 0;
};

class DiskModel {
public:
    static constexpr unsigned kMaxWriters = 32;
    static constexpr unsigned kChunkWindows = 4;        // windows per estimate of a[W]
    static constexpr std::size_t kKnownChunks = 4;      // estimates before a count is "known"
    static constexpr double kHalfLifeWindows = 240;     // b and c forget with a 2-minute half-life
    static constexpr std::int64_t kChunkMemoryNs = 180'000'000'000;   // a[W] estimates kept 3 minutes
    static constexpr double kMixSeconds = 60;           // the current mix: about the last minute

    // Every window, saturated or not: the mix being written.
    void observe_mix(const Window& window);

    // A window in which writing was the limit, at `writers` writers.
    void observe(unsigned writers, const Window& window, std::int64_t at_ns);

    // Starts a new chunk: call when the writer count changes or a window is not saturated.
    void break_chunk() noexcept { pending_ = {}; pending_windows_ = 0; }

    struct Estimate {
        double per_file = 0;      // a[W], seconds
        double uncertainty = 0;   // standard error of per_file
        std::size_t chunks = 0;
    };
    [[nodiscard]] std::optional<Estimate> estimate(unsigned writers, std::int64_t now_ns) const;
    [[nodiscard]] bool known(unsigned writers, std::int64_t now_ns) const;

    // Disk seconds that one second of the current mix takes at `writers`, and its uncertainty.
    struct Prediction {
        double seconds = 0;
        double uncertainty = 0;
    };
    [[nodiscard]] std::optional<Prediction> predict(unsigned writers, std::int64_t now_ns) const;

    [[nodiscard]] double seconds_per_byte_written() const noexcept { return b_; }
    [[nodiscard]] double seconds_per_byte_read() const noexcept { return c_; }

private:
    struct Chunk {
        Window sum;
        std::int64_t at_ns = 0;
    };
    struct PerCount {   // forgetting sums of the least-squares fit, per writer count
        double ff = 0, fr = 0, fq = 0, fy = 0;
        std::deque<Chunk> chunks;
    };

    void solve();

    std::array<PerCount, kMaxWriters + 1> counts_{};
    double rr_ = 0, rq_ = 0, qq_ = 0, ry_ = 0, qy_ = 0;   // shared sums
    double b_ = 0, c_ = 0;
    std::array<double, kMaxWriters + 1> a_{};
    double scale_ = 0;   // typical size of a window's residual, for down-weighting stalls

    Window pending_;
    unsigned pending_windows_ = 0;
    unsigned pending_writers_ = 0;

    Window mix_;   // exponentially weighted, per second
    bool have_mix_ = false;
};

} // namespace npk::pipeline
