#include "stages.hpp"

#include "disk_model.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

namespace npk::pipeline {

namespace {

using namespace std::chrono_literals;

constexpr auto kWindow = 500ms;                        // one look at the clocks
constexpr std::int64_t kSecond = 1'000'000'000;
constexpr std::int64_t kBlockPause = 2 * kSecond;      // between read block changes
constexpr std::int64_t kBlockWarmUp = 2 * kSecond;     // before the first read block change
constexpr double kBusy = 0.85;                         // a stage is the limit above this share of working time
constexpr double kStarved = 0.5;                       // decoders mostly waiting for input
constexpr double kBlocked = 0.5;                       // decoders mostly waiting for output space

// Writer search (docs/pipeline.md §7).
constexpr double kSure = 2;                            // standard errors a difference must exceed
constexpr std::size_t kMaxProbeChunks = 12;            // after this many estimates, decide on what is known
constexpr std::int64_t kProbeTimeout = 40 * kSecond;   // writing stopped being the limit during a probe
constexpr std::int64_t kRecheck = 120 * kSecond;       // look at the neighbours again after settling

std::string format(const char* fmt, auto... args)
{
    char text[256];
    std::snprintf(text, sizeof text, fmt, args...);
    return text;
}

StageTimes sum(const std::deque<StageClock>& clocks)
{
    StageTimes total;
    for (const StageClock& clock : clocks) total += clock.times();
    return total;
}

// Finds the writer count with the lowest predicted time for the current mix:
// expand with growing steps while it helps (+1, +2, +4 ...), then refine
// between the last two tries one by one; downwards the same way.
class WriterSearch {
public:
    enum class Verdict { better, worse, undecided };

    WriterSearch(const DiskModel& model, const Context& ctx) : model_(model), ctx_(ctx) {}

    // The count to run at now.
    [[nodiscard]] unsigned target() const noexcept { return probing_ ? candidate_ : best_; }
    [[nodiscard]] bool probing() const noexcept { return probing_; }
    [[nodiscard]] bool settled() const noexcept { return settled_; }
    [[nodiscard]] std::int64_t next_check() const noexcept { return next_check_; }

    void start_at(unsigned writers) { best_ = writers; }

    void begin(std::int64_t now)
    {
        settled_ = false;
        direction_ = +1;
        step_ = 1;
        refining_ = false;
        improved_ = false;
        turned_ = false;
        propose(now);
    }

    // Called every window while probing.
    void update(std::int64_t now)
    {
        if (!probing_) return;
        const Verdict verdict = judge(now);
        if (verdict == Verdict::undecided) {
            if (now - probe_started_ < kProbeTimeout) return;
            ctx_.log(format("tuner: not enough data at %u writers, staying at %u", candidate_, best_));
            return finish(now);
        }
        if (verdict == Verdict::better) {
            best_ = candidate_;
            improved_ = true;
            if (refining_) {
                step_ /= 2;
                if (step_ == 0) return finish(now);
            } else {
                step_ *= 2;
            }
        } else if (step_ > 1) {
            refining_ = true;   // the best count is between best_ and the candidate
            step_ /= 2;
        } else if (!improved_ && !turned_) {
            turn();             // the first step this way didn't help: try the other way
        } else {
            return finish(now);
        }
        propose(now);
    }

private:
    void turn()
    {
        direction_ = -direction_;
        turned_ = true;
        refining_ = false;
        step_ = 1;
    }

    void propose(std::int64_t now)
    {
        const int candidate = static_cast<int>(best_) + direction_ * static_cast<int>(step_);
        if (candidate < 1 || candidate > static_cast<int>(ctx_.profile.max)) {   // off the end: as if worse
            if (step_ > 1) {
                refining_ = true;
                step_ /= 2;
                return propose(now);
            }
            if (!improved_ && !turned_) {
                turn();
                return propose(now);
            }
            return finish(now);
        }
        candidate_ = static_cast<unsigned>(candidate);
        probing_ = true;
        probe_started_ = now;
        ctx_.log(format("tuner: trying %u writers (best so far %u)", candidate_, best_));
    }

    void finish(std::int64_t now)
    {
        probing_ = false;
        settled_ = true;
        next_check_ = now + kRecheck;
        const auto e = model_.estimate(best_, now);
        const double b = model_.seconds_per_byte_written();
        ctx_.log(format("tuner: settled on %u writers (%.0f us per file, %.0f MB/s)", best_,
                        e ? e->per_file * 1e6 : 0.0, b > 0 ? 1 / b / 1e6 : 0.0));
    }

    [[nodiscard]] Verdict judge(std::int64_t now) const
    {
        const auto estimate = model_.estimate(candidate_, now);
        if (!estimate || estimate->chunks < DiskModel::kKnownChunks) return Verdict::undecided;
        const auto at_best = model_.predict(best_, now);
        const auto at_candidate = model_.predict(candidate_, now);
        if (!at_best || !at_candidate || at_best->seconds <= 0) return Verdict::undecided;

        const double change = at_candidate->seconds / at_best->seconds - 1;   // < 0: the candidate is faster
        const double doubt = kSure * std::hypot(at_best->uncertainty, at_candidate->uncertainty) / at_best->seconds;
        const bool fewer = candidate_ < best_;
        const double tie = ctx_.profile.tie;
        Verdict verdict = Verdict::undecided;
        if (change < -tie && change + doubt < 0) {
            verdict = Verdict::better;
        } else if (change > tie && change - doubt > 0) {
            verdict = Verdict::worse;
        } else if (std::abs(change) <= tie && doubt <= tie) {
            verdict = fewer ? Verdict::better : Verdict::worse;   // the same: fewer writers win
        } else if (estimate->chunks >= kMaxProbeChunks) {     // as sure as it gets: go by the estimate
            verdict = change < -tie || (fewer && change <= tie) ? Verdict::better : Verdict::worse;
        }
        if (verdict != Verdict::undecided) {
            ctx_.log(format("tuner: %u writers %s than %u (%+.0f %% time, +-%.0f %%)", candidate_,
                            verdict == Verdict::better ? "better" : "not better", best_, change * 100, doubt * 100));
        }
        return verdict;
    }

    const DiskModel& model_;
    const Context& ctx_;
    unsigned best_ = 2;
    unsigned candidate_ = 2;
    int direction_ = +1;
    unsigned step_ = 1;
    bool refining_ = false;   // no more growing steps: halving towards the best count
    bool improved_ = false;   // this direction found a better count
    bool turned_ = false;     // the other direction has been tried
    bool probing_ = false;
    bool settled_ = false;
    std::int64_t probe_started_ = 0;
    std::int64_t next_check_ = 0;
};

} // namespace

Tuner::Sample Tuner::take() const
{
    Sample sample;
    sample.reader = reader_.clock().times();
    sample.decoders = sum(decoders_.clocks());
    sample.writers = sum(writers_.clocks());
    // Bytes handed to the OS. On Linux the writeback tracker keeps at most a few
    // dozen MB in the page cache and makes writers wait beyond that, so this rate
    // is the disk's rate; it has no lag, unlike bytes_on_disk.
    sample.bytes = ctx_.progress.bytes_written.load(std::memory_order_relaxed);
    sample.files = ctx_.progress.files_written.load(std::memory_order_relaxed);
    sample.bytes_read = ctx_.progress.bytes_read.load(std::memory_order_relaxed);
    sample.at_ns = StageClock::now_ns();
    return sample;
}

void Tuner::set_writers(unsigned count)
{
    ctx_.tunables.writers_allowed.store(count, std::memory_order_relaxed);
    ctx_.progress.writers_active.store(count, std::memory_order_relaxed);
    writers_.allowed_changed();
}

void Tuner::trace(const Sample& now, const Sample& previous) const
{
    const StageTimes reader = now.reader - previous.reader;
    const StageTimes decoders = now.decoders - previous.decoders;
    const StageTimes writers = now.writers - previous.writers;
    const auto s = [](std::int64_t ns) { return static_cast<double>(ns) / kSecond; };
    ctx_.log(format("trace %.3f %u %llu %llu %.3f %.3f %.3f %.3f %.3f %llu",
                    s(now.at_ns - previous.at_ns), ctx_.tunables.writers_allowed.load(std::memory_order_relaxed),
                    static_cast<unsigned long long>(now.bytes - previous.bytes),
                    static_cast<unsigned long long>(now.files - previous.files), s(writers[StageState::working]),
                    s(writers[StageState::waiting_input]), s(decoders[StageState::working]),
                    s(decoders[StageState::waiting_output]), reader.busy(),
                    static_cast<unsigned long long>(now.bytes_read - previous.bytes_read)));
}

void Tuner::run(std::stop_token stop)
{
    std::mutex sleep_mutex;
    std::condition_variable_any sleeper;
    const auto sleep = [&] {
        std::unique_lock lock(sleep_mutex);
        sleeper.wait_for(lock, stop, kWindow, [] { return false; });
    };

    if (!ctx_.options.writer_schedule.empty() || !ctx_.options.adaptive) {   // tests: follow a schedule, or only trace
        const auto& schedule = ctx_.options.writer_schedule;
        Sample previous = take();
        const std::int64_t start = previous.at_ns;
        while (!stop.stop_requested()) {
            if (!schedule.empty()) {
                const auto slot = static_cast<std::size_t>(static_cast<double>(previous.at_ns - start) / kSecond /
                                                           ctx_.options.schedule_period);
                const unsigned wanted = schedule[slot % schedule.size()];
                if (wanted != ctx_.tunables.writers_allowed.load(std::memory_order_relaxed)) set_writers(wanted);
            }
            sleep();
            const Sample now = take();
            if (ctx_.options.trace) trace(now, previous);
            previous = now;
        }
        return;
    }

    DiskModel model;
    WriterSearch search(model, ctx_);
    search.start_at(ctx_.tunables.writers_allowed.load(std::memory_order_relaxed));
    ctx_.log(format("tuner: searching as for %s: from %u writers, at most %u, gains under %.0f %% don't count",
                    ctx_.disk == DiskKind::hdd ? "a hard disk" : "an SSD", ctx_.tunables.writers_allowed.load(),
                    ctx_.profile.max, ctx_.profile.tie * 100));
    bool settle = true;   // the window after a change of writer count doesn't count

    Sample previous = take();
    std::int64_t next_block_change = previous.at_ns + kBlockWarmUp;

    while (!stop.stop_requested()) {
        sleep();
        if (stop.stop_requested()) break;

        const Sample now = take();
        if (ctx_.options.trace) trace(now, previous);
        const double seconds = static_cast<double>(now.at_ns - previous.at_ns) / kSecond;
        if (seconds <= 0) continue;
        const StageTimes reader = now.reader - previous.reader;
        const StageTimes decoders = now.decoders - previous.decoders;
        const StageTimes writers = now.writers - previous.writers;
        const Window window{seconds, static_cast<double>(now.files - previous.files),
                            static_cast<double>(now.bytes - previous.bytes),
                            static_cast<double>(now.bytes_read - previous.bytes_read)};
        previous = now;

        // Reading is the limit: read bigger blocks (fewer calls, fewer seeks between writes).
        const std::size_t block = ctx_.tunables.read_block.load(std::memory_order_relaxed);
        if (reader.busy() > kBusy && decoders.share(StageState::waiting_input) > kStarved &&
            block < Tunables::kMaxReadBlock && now.at_ns >= next_block_change) {
            const std::size_t bigger = std::min(block * 2, Tunables::kMaxReadBlock);
            ctx_.tunables.read_block.store(bigger, std::memory_order_relaxed);
            ctx_.progress.read_block_kib.store(static_cast<std::uint32_t>(bigger >> 10), std::memory_order_relaxed);
            ctx_.log(format("tuner: read block %zu -> %zu MiB (reading is the limit)", block >> 20, bigger >> 20));
            next_block_change = now.at_ns + kBlockPause;
        }

        // --- Writers ---
        // Writing is the limit when the writers are busy, or when the decoders mostly
        // wait to hand work on (many tiny files: much of a writer's time goes into
        // taking jobs, not into "working"). Only such windows say what the disk costs.
        model.observe_mix(window);
        const unsigned current = ctx_.tunables.writers_allowed.load(std::memory_order_relaxed);
        const bool write_bound = writers.busy() > kBusy || decoders.share(StageState::waiting_output) > kBlocked;
        if (settle || !write_bound) {
            model.break_chunk();
        } else {
            model.observe(current, window, now.at_ns);
        }
        settle = false;

        if (search.probing()) {
            search.update(now.at_ns);
        } else if (write_bound && model.known(current, now.at_ns) &&
                   (!search.settled() || now.at_ns >= search.next_check())) {
            search.begin(now.at_ns);   // the first search, or looking at the neighbours again
        }
        if (search.target() != current) {
            set_writers(search.target());
            settle = true;
        }
    }
}

} // namespace npk::pipeline
