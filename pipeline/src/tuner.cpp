#include "stages.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <string>
#include <utility>

namespace npk::pipeline {

namespace {

using namespace std::chrono_literals;

constexpr auto kWindow = 500ms;                        // one look at the clocks
constexpr std::int64_t kSecond = 1'000'000'000;
constexpr std::int64_t kWarmUp = 2 * kSecond;          // first probe: after the first cache fill
constexpr std::int64_t kProbePause = 30 * kSecond;     // after a probe that did not help
constexpr std::int64_t kNoisyPause = 30 * kSecond;     // after giving up on a disk too uneven to measure
constexpr std::int64_t kBlockPause = 2 * kSecond;      // between read block changes
constexpr std::int64_t kArchivePause = 10 * kSecond;   // a new big archive allows a probe this long after a decision
constexpr double kBusy = 0.85;                         // a stage is the limit above this share of working time
constexpr double kStarved = 0.5;                       // decoders mostly waiting for input
constexpr double kBlocked = 0.5;                       // decoders mostly waiting for output space
constexpr double kMinGain = 0.05;                      // a probe must gain more than 5 %, and more than the noise
constexpr double kFileCost = 64 * 1024;                // one file is worth this many bytes in the score
constexpr std::uint64_t kBigArchive = std::uint64_t{64} << 20;

// Throughput is compared in samples of `span` windows. A disk that stalls in
// bursts makes short samples useless, so the span doubles while the samples
// vary too much: 2 s, 4 s, 8 s.
constexpr unsigned kFirstSpan = 4;
constexpr unsigned kMaxSpan = 16;
constexpr std::size_t kBaselineSamples = 4;            // steady samples a probe is compared against
constexpr std::size_t kMeasureSamples = 4;             // samples a probe is measured over
constexpr double kMaxNoise = 0.35;                     // stdev / mean above this: samples can't be compared

// Mean and coefficient of variation.
std::pair<double, double> mean_and_noise(const std::deque<double>& samples)
{
    double mean = 0;
    for (const double s : samples) mean += s;
    mean /= static_cast<double>(samples.size());
    if (mean <= 0) return {0, 1};
    double variance = 0;
    for (const double s : samples) variance += (s - mean) * (s - mean);
    variance /= static_cast<double>(samples.size());
    return {mean, std::sqrt(variance) / mean};
}

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
    ctx_.log(format("trace %.3f %u %llu %llu %.3f %.3f %.3f %.3f %.3f",
                    s(now.at_ns - previous.at_ns), ctx_.tunables.writers_allowed.load(std::memory_order_relaxed),
                    static_cast<unsigned long long>(now.bytes - previous.bytes),
                    static_cast<unsigned long long>(now.files - previous.files), s(writers[StageState::working]),
                    s(writers[StageState::waiting_input]), s(decoders[StageState::working]),
                    s(decoders[StageState::waiting_output]), reader.busy()));
}

void Tuner::run(std::stop_token stop)
{
    if (!ctx_.options.writer_schedule.empty() || !ctx_.options.adaptive) {   // tests: follow a schedule, or only trace
        const auto& schedule = ctx_.options.writer_schedule;
        Sample previous = take();
        const std::int64_t start = previous.at_ns;
        std::mutex sleep_mutex;
        std::condition_variable_any sleeper;
        while (!stop.stop_requested()) {
            if (!schedule.empty()) {
                const auto slot = static_cast<std::size_t>(static_cast<double>(previous.at_ns - start) / kSecond /
                                                           ctx_.options.schedule_period);
                const unsigned wanted = schedule[slot % schedule.size()];
                if (wanted != ctx_.tunables.writers_allowed.load(std::memory_order_relaxed)) set_writers(wanted);
            }
            {
                std::unique_lock lock(sleep_mutex);
                sleeper.wait_for(lock, stop, kWindow, [] { return false; });
            }
            const Sample now = take();
            if (ctx_.options.trace) trace(now, previous);
            previous = now;
        }
        return;
    }

    // A probe is measured before -> after -> before again, and the "after" is
    // compared with both "before" periods together. A slow change in conditions
    // (the start-up fill, a different kind of archive) then shows up as variation
    // between the two "before" periods instead of as a gain or a loss.
    enum class Phase { steady, settle, probe, back };
    Phase phase = Phase::steady;
    Phase after_settle = Phase::steady;
    unsigned span = kFirstSpan;

    // The sample being collected.
    double sample_score = 0;
    double sample_busy = 0;      // writers' working share
    double sample_blocked = 0;   // decoders' waiting-for-output share
    unsigned sample_windows = 0;

    std::deque<double> recent;     // steady samples at the current writer count: the first "before"
    std::deque<double> measured;   // samples at the probed count
    std::deque<double> back;       // samples after returning: the second "before"
    unsigned previous_writers = 0;
    unsigned probe_writers = 0;
    bool go_down = false;
    bool gave_up_logged = false;
    const ArchiveSource* last_source = nullptr;
    std::int64_t last_decision = 0;   // when a probe last ended

    Sample previous = take();
    std::int64_t next_probe = previous.at_ns + kWarmUp;
    std::int64_t next_block_change = previous.at_ns + kWarmUp;

    const auto change_to = [&](unsigned writers, Phase then) {
        set_writers(writers);
        phase = Phase::settle;
        after_settle = then;
    };

    std::mutex sleep_mutex;
    std::condition_variable_any sleeper;
    while (!stop.stop_requested()) {
        {
            std::unique_lock lock(sleep_mutex);
            sleeper.wait_for(lock, stop, kWindow, [] { return false; });
        }
        if (stop.stop_requested()) break;

        const Sample now = take();
        const double seconds = static_cast<double>(now.at_ns - previous.at_ns) / kSecond;
        if (seconds <= 0) continue;
        if (ctx_.options.trace) trace(now, previous);
        const StageTimes reader = now.reader - previous.reader;
        const StageTimes decoders = now.decoders - previous.decoders;
        const StageTimes writers = now.writers - previous.writers;
        const double score = (static_cast<double>(now.bytes - previous.bytes) +
                              kFileCost * static_cast<double>(now.files - previous.files)) / seconds;
        previous = now;

        // A new big archive may behave differently: allow a probe sooner, but not
        // right after a decision (a big installation starts one every few seconds).
        if (const ArchiveSource* source = ctx_.progress.current_source.load(std::memory_order_relaxed);
            source != last_source) {
            last_source = source;
            if (source != nullptr && source->packed_bytes >= kBigArchive) {
                next_probe = std::min(next_probe, std::max(now.at_ns, last_decision + kArchivePause));
            }
        }

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

        // --- Writers: collect a sample of `span` windows, then decide. ---
        if (phase == Phase::settle) {   // the window right after a change doesn't count
            phase = after_settle;
            sample_score = sample_busy = sample_blocked = 0;
            sample_windows = 0;
            continue;
        }
        sample_score += score;
        sample_busy += writers.busy();
        sample_blocked += decoders.share(StageState::waiting_output);
        if (++sample_windows < span) continue;
        const double sample = sample_score / sample_windows;
        const double busy = sample_busy / sample_windows;
        const double blocked = sample_blocked / sample_windows;
        sample_score = sample_busy = sample_blocked = 0;
        sample_windows = 0;

        const unsigned current = ctx_.tunables.writers_allowed.load(std::memory_order_relaxed);
        if (phase == Phase::steady) {
            recent.push_back(sample);
            if (recent.size() > kBaselineSamples) recent.pop_front();
            // Writing is the limit when the writers are busy (also when a hard disk is shared
            // with reading), or when the decoders mostly wait to hand work on (many tiny
            // files: much of a writer's time goes into taking jobs, not into "working").
            const bool write_bound = busy > kBusy || blocked > kBlocked;
            if (now.at_ns < next_probe || !write_bound || recent.size() < kBaselineSamples) continue;

            const auto [mean, noise] = mean_and_noise(recent);
            if (mean <= 0) continue;
            if (noise > kMaxNoise) {
                if (span < kMaxSpan) {   // too uneven: measure over longer samples
                    span *= 2;
                    recent.clear();
                    ctx_.log(format("tuner: throughput uneven (%.0f %% variation), comparing over %.0f s samples",
                                    noise * 100, span * 0.5));
                } else {
                    if (!gave_up_logged) {
                        ctx_.log(format("tuner: throughput too uneven to compare (%.0f %%), keeping %u writers",
                                        noise * 100, current));
                        gave_up_logged = true;
                    }
                    next_probe = now.at_ns + kNoisyPause;
                }
                continue;
            }

            // One writer at a time, so a sweet spot at 3 or 5 is not stepped over.
            const unsigned target = go_down ? std::max(current - 1, 1u) : std::min(current + 1, Tunables::kMaxWriters);
            if (target == current) {   // at a bound: look the other way next time
                go_down = !go_down;
                next_probe = now.at_ns + kProbePause;
                continue;
            }
            previous_writers = current;
            probe_writers = target;
            measured.clear();
            ctx_.log(format("tuner: trying %u writers instead of %u", target, current));
            change_to(target, Phase::probe);
            continue;
        }

        if (phase == Phase::probe) {
            measured.push_back(sample);
            if (measured.size() < kMeasureSamples) continue;
            back.clear();
            change_to(previous_writers, Phase::back);   // measure "before" once more
            continue;
        }

        // Phase::back
        back.push_back(sample);
        if (back.size() < kMeasureSamples) continue;
        std::deque<double> before = recent;
        before.insert(before.end(), back.begin(), back.end());
        const auto [base, base_noise] = mean_and_noise(before);
        const auto [result, result_noise] = mean_and_noise(measured);
        const double change = (result / base - 1) * 100;
        // The gain must beat kMinGain and the uncertainty of the difference. A low
        // bar on purpose: wrongly keeping a step near the best count costs a few
        // percent, wrongly rejecting one can leave a fast disk at half speed.
        const double uncertainty = std::sqrt(base_noise * base_noise / static_cast<double>(before.size()) +
                                             result_noise * result_noise / static_cast<double>(measured.size()));
        const double needed = std::max(kMinGain, uncertainty);
        last_decision = now.at_ns;
        phase = Phase::steady;

        if (base_noise > kMaxNoise || result_noise > kMaxNoise) {
            ctx_.log(format("tuner: writers %u -> %u inconclusive (%.0f %% / %.0f %% variation), staying at %u",
                            previous_writers, probe_writers, base_noise * 100, result_noise * 100, previous_writers));
        } else if (result > base * (1 + needed)) {
            ctx_.log(format("tuner: writers %u -> %u (%+.0f %%)", previous_writers, probe_writers, change));
            change_to(probe_writers, Phase::steady);
            next_probe = now.at_ns;   // keep going the same way,
            recent = measured;        // with what was measured there as the first "before"
            continue;
        } else {
            ctx_.log(format("tuner: writers %u -> %u did not help (%+.0f %%, needed %+.0f %%), staying at %u",
                            previous_writers, probe_writers, change, needed * 100, previous_writers));
        }
        go_down = !go_down;
        next_probe = now.at_ns + kProbePause;
        recent = back;   // fresh samples at the current count
    }
}

} // namespace npk::pipeline
