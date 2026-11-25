#include "simulated_disk.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <thread>

namespace npk::sim {

namespace {

using Seconds = std::chrono::duration<double>;

std::chrono::steady_clock::duration to_duration(double seconds)
{
    return std::chrono::duration_cast<std::chrono::steady_clock::duration>(Seconds(seconds));
}

// "80us" -> 8e-5 and so on; the suffix decides the unit.
bool parse(std::string_view text, double& value)
{
    const char* end = text.data() + text.size();
    auto [rest, error] = std::from_chars(text.data(), end, value);
    if (error != std::errc{}) return false;
    const std::string_view unit(rest, static_cast<std::size_t>(end - rest));
    struct Unit { std::string_view name; double factor; };
    static constexpr Unit units[] = {
        {"", 1},      {"s", 1},          {"ms", 1e-3},        {"us", 1e-6},
        {"B/s", 1},   {"KB/s", 1e3},     {"MB/s", 1e6},       {"GB/s", 1e9},
        {"B", 1},     {"KiB", 1024},     {"MiB", 1048576},    {"GiB", 1073741824},
        {"%", 0.01},
    };
    for (const Unit& u : units) {
        if (unit == u.name) {
            value *= u.factor;
            return true;
        }
    }
    return false;
}

} // namespace

DiskParams ssd()
{
    DiskParams p;
    p.channels = 8;
    p.file_latency = 80e-6;
    p.contention = 0.02;
    p.bandwidth = 1500e6;
    p.switch_cost = 0;
    p.stall_every = 5;
    p.stall_length = 0.020;
    return p;
}

DiskParams hdd()
{
    DiskParams p;
    p.channels = 1;
    p.file_latency = 500e-6;
    p.bandwidth = 150e6;
    p.switch_cost = 1e-3;
    p.shared_with_input = true;
    p.stall_every = 10;
    p.stall_length = 0.300;
    return p;
}

std::string set_param(DiskParams& p, std::string_view assignment)
{
    const auto equals = assignment.find('=');
    if (equals == std::string_view::npos) return "expected name=value, got " + std::string(assignment);
    const std::string_view name = assignment.substr(0, equals);
    const std::string_view text = assignment.substr(equals + 1);

    if (name == "shared_with_input") {
        if (text == "yes" || text == "1" || text == "true") p.shared_with_input = true;
        else if (text == "no" || text == "0" || text == "false") p.shared_with_input = false;
        else return "shared_with_input takes yes or no";
        return {};
    }
    double value = 0;
    if (!parse(text, value) || value < 0) return "bad value for " + std::string(name) + ": " + std::string(text);
    if (name == "channels") p.channels = std::clamp(static_cast<unsigned>(value), 1u, 1024u);
    else if (name == "file_latency") p.file_latency = value;
    else if (name == "contention") p.contention = value;
    else if (name == "bandwidth") p.bandwidth = std::max(value, 1.0);
    else if (name == "chunk") p.chunk = std::max<std::size_t>(static_cast<std::size_t>(value), 4096);
    else if (name == "switch_cost") p.switch_cost = value;
    else if (name == "stall_every") p.stall_every = value;
    else if (name == "stall_length") p.stall_length = value;
    else if (name == "seed") p.seed = static_cast<std::uint32_t>(value);
    else return "unknown parameter " + std::string(name);
    return {};
}

std::string describe(const DiskParams& p)
{
    char text[400];
    std::snprintf(text, sizeof text,
                  "channels=%u file_latency=%.0fus contention=%.0f%% bandwidth=%.0fMB/s chunk=%zuKiB "
                  "switch_cost=%.2fms shared_with_input=%s stall_every=%.1fs stall_length=%.0fms",
                  p.channels, p.file_latency * 1e6, p.contention * 100, p.bandwidth / 1e6, p.chunk >> 10,
                  p.switch_cost * 1e3, p.shared_with_input ? "yes" : "no", p.stall_every, p.stall_length * 1e3);
    return text;
}

SimulatedDisk::SimulatedDisk(const DiskParams& params)
    : params_(params)
    , streams_(kMaxStreams, Clock::now())
    , slots_(params.channels, Clock::now())
    , pipe_free_(Clock::now())
    , random_(params.seed)
{
    next_stall_ = params_.stall_every > 0
                      ? pipe_free_ + to_duration(std::exponential_distribution<double>(1 / params_.stall_every)(random_))
                      : Clock::time_point::max();
}

SimulatedDisk::Clock::time_point& SimulatedDisk::clock_of(unsigned stream)
{
    Clock::time_point& clock = streams_[std::min(stream, kReader)];
    const auto now = Clock::now();
    if (now - clock > kIdle) clock = now;   // was waiting for work, not for the disk
    return clock;
}

// When a stream may start using a shared part of the disk. A stream ahead of
// the real clock starts now, so its lead doesn't hold the others back; one that
// overslept may claim the time it lost.
SimulatedDisk::Clock::time_point SimulatedDisk::ready(Clock::time_point clock)
{
    return std::min(clock, Clock::now());
}

void SimulatedDisk::catch_up(Clock::time_point until)
{
    if (until - Clock::now() > kSlack) std::this_thread::sleep_until(until);
}

void SimulatedDisk::write_file(unsigned writer, std::size_t bytes)
{
    writer = std::min(writer, kReader - 1);
    const unsigned writers = in_flight_.fetch_add(1, std::memory_order_relaxed) + 1;
    Clock::time_point& clock = clock_of(writer);
    if (params_.file_latency > 0) {
        const unsigned extra = writers > params_.channels ? writers - params_.channels : 0;
        const auto latency = to_duration(params_.file_latency * (1 + params_.contention * extra));
        {
            std::lock_guard lock(mutex_);
            auto slot = std::min_element(slots_.begin(), slots_.end());
            const auto start = std::max(*slot, ready(clock));
            *slot = start + latency;
            clock = std::max(clock + latency, *slot);
        }
        catch_up(clock);
    }
    transfer(writer, bytes);
    in_flight_.fetch_sub(1, std::memory_order_relaxed);
}

void SimulatedDisk::read(std::size_t bytes)
{
    if (params_.shared_with_input) {
        clock_of(kReader);
        transfer(kReader, bytes);
    }
}

void SimulatedDisk::transfer(unsigned stream, std::size_t bytes)
{
    Clock::time_point& clock = streams_[stream];
    while (bytes > 0) {
        const std::size_t piece = std::min(bytes, params_.chunk);
        bytes -= piece;
        {
            std::lock_guard lock(mutex_);
            const Clock::time_point start = std::max(pipe_free_, ready(clock));
            Clock::time_point end = start + to_duration(static_cast<double>(piece) / params_.bandwidth);
            if (last_stream_ != stream && last_stream_ != kNobody) end += to_duration(params_.switch_cost);
            last_stream_ = stream;
            if (start >= next_stall_) {
                end += to_duration(params_.stall_length);
                next_stall_ = end + to_duration(std::exponential_distribution<double>(1 / params_.stall_every)(random_));
            }
            pipe_free_ = end;
            clock = std::max(clock + (end - start), end);
        }
        catch_up(clock);
    }
}

} // namespace npk::sim
