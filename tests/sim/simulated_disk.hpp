#pragma once

// A modelled disk for testing the tuner: see docs/simulated_disk.md.

#include "npk/pipeline/fake_disk.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <random>
#include <vector>
#include <string>
#include <string_view>

namespace npk::sim {

struct DiskParams {
    unsigned channels = 1;          // files whose file cost is paid at the same time
    double file_latency = 0;        // seconds per file: create, open, close
    double contention = 0;          // extra file latency per writer above `channels`, as a fraction
    double bandwidth = 1e8;         // bytes per second through the shared pipe
    std::size_t chunk = 1 << 20;    // bytes per piece in the pipe
    double switch_cost = 0;         // seconds when the pipe changes stream
    bool shared_with_input = false; // archive reads go through the pipe too
    double stall_every = 0;         // mean seconds of disk time between stalls; 0 = never
    double stall_length = 0;        // seconds per stall
    std::uint32_t seed = 1;
};

DiskParams ssd();
DiskParams hdd();

// Sets one parameter from "name=value". Values take units: 80us, 1ms, 2s,
// 1500MB/s, 1MiB, yes/no. Returns an error message, or "" on success.
std::string set_param(DiskParams& params, std::string_view assignment);
std::string describe(const DiskParams& params);

// Works in virtual time. Every stream (each writer, and the reader) has its own
// clock that moves by modelled costs, and sleeps only once it is more than
// kSlack ahead of the real clock. The shared parts (channels, pipe) book time
// from the real clock, so one stream's lead doesn't drag the others along. Short sleeps overshoot by a lot on most
// systems; this way the overshoot is paid back by later operations instead of
// slowing the modelled disk down.
class SimulatedDisk final : public pipeline::FakeDisk {
public:
    explicit SimulatedDisk(const DiskParams& params);

    void write_file(unsigned writer, std::size_t bytes) override;
    void read(std::size_t bytes) override;

private:
    using Clock = std::chrono::steady_clock;
    static constexpr unsigned kMaxStreams = 64;
    static constexpr unsigned kReader = kMaxStreams - 1;
    static constexpr unsigned kNobody = kMaxStreams;

    // A stream that has been idle for longer than this starts again from now.
    static constexpr auto kIdle = std::chrono::milliseconds(3);
    static constexpr auto kSlack = std::chrono::milliseconds(1);

    Clock::time_point& clock_of(unsigned stream);
    void transfer(unsigned stream, std::size_t bytes);
    static Clock::time_point ready(Clock::time_point clock);
    static void catch_up(Clock::time_point until);

    DiskParams params_;
    std::atomic<unsigned> in_flight_{0};
    std::vector<Clock::time_point> streams_;   // each entry used by one thread only

    std::mutex mutex_;                          // guards everything below
    std::vector<Clock::time_point> slots_;     // when each file-cost channel is free
    Clock::time_point pipe_free_;
    Clock::time_point next_stall_;
    unsigned last_stream_ = kNobody;
    std::mt19937 random_;
};

} // namespace npk::sim
