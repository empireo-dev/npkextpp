#pragma once

// The pipeline's threads (private to npk_pipeline). See docs/pipeline.md §4 and §7.

#include "context.hpp"
#include "npk/pipeline/stage_clock.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace npk::pipeline {

// Stage 1, one thread. Walks the sources in plan order and each archive's
// entries by offset, merges neighbours into reads of Tunables::read_block
// bytes, and cuts every chunk into ~1 MiB DecodeTasks. Waits on the read budget.
class Reader {
public:
    static constexpr std::uint64_t kMaxGap = std::uint64_t{512} << 10;   // unwanted bytes read through
    static constexpr std::uint64_t kTaskBytes = std::uint64_t{1} << 20;

    explicit Reader(Context& context) : ctx_(context) {}

    void run();   // closes the decode queue when done

    [[nodiscard]] const StageClock& clock() const noexcept { return clock_; }

private:
    void read_source(ArchiveSource& source, std::uint32_t index);
    bool emit(std::shared_ptr<ReadChunk> chunk);

    Context& ctx_;
    StageClock clock_;
};

// Stage 2, one thread per hardware thread, each with its own npk::Decoder.
// The last thread to finish closes the write queue.
class DecoderPool {
public:
    explicit DecoderPool(Context& context) : ctx_(context) {}
    ~DecoderPool() { join(); }

    void start(unsigned threads);
    void join();

    [[nodiscard]] const std::deque<StageClock>& clocks() const noexcept { return clocks_; }

private:
    void run(StageClock& clock);

    Context& ctx_;
    std::deque<StageClock> clocks_;   // deque: elements never move
    std::vector<std::jthread> threads_;
    std::atomic<unsigned> running_{0};
};

// Stage 3. Tunables::kMaxWriters threads; the first writers_allowed take jobs,
// the others stay parked, so the tuner changes the number of writers without
// creating threads.
class WriterPool {
public:
    explicit WriterPool(Context& context) : ctx_(context) {}
    ~WriterPool() { join(); }

    void start();
    void allowed_changed();   // wake parked writers after Tunables::writers_allowed changed
    void join();              // after the write queue is closed

    [[nodiscard]] const std::deque<StageClock>& clocks() const noexcept { return clocks_; }

private:
    void run(unsigned index, StageClock& clock);
    void write(unsigned index, WriteJob& job);
    void written(WriteJob& job);   // marks the entry written and counts it
    void fail_write(WriteJob& job, const std::filesystem::path& path, Error error);

    Context& ctx_;
    std::deque<StageClock> clocks_;
    std::vector<std::jthread> threads_;
    std::mutex park_mutex_;
    std::condition_variable_any park_;
};

// Every 500 ms: publishes how each stage spent its time (for the UI) and, when
// adaptive, adjusts the number of writers and the read block size (§7).
class Tuner {
public:
    Tuner(Context& context, const Reader& reader, const DecoderPool& decoders, WriterPool& writers)
        : ctx_(context)
        , reader_(reader)
        , decoders_(decoders)
        , writers_(writers)
    {
    }

    void run(std::stop_token stop);

private:
    struct Sample {
        StageTimes reader, decoders, writers;
        std::uint64_t bytes = 0;
        std::uint64_t files = 0;
        std::uint64_t bytes_read = 0;
        std::uint64_t blocks_read = 0;
        std::uint64_t files_decoded = 0;
        std::int64_t at_ns = 0;
    };
    static constexpr std::size_t kLoadWindows = 4;   // Progress::reader/decoders/writers cover the last 2 s

    [[nodiscard]] Sample take() const;
    void publish(const Sample& now);   // stage loads for the UI
    void set_writers(unsigned count);
    void trace(const Sample& now, const Sample& previous) const;

    Context& ctx_;
    const Reader& reader_;
    const DecoderPool& decoders_;
    WriterPool& writers_;
    std::deque<Sample> history_;
};

} // namespace npk::pipeline
