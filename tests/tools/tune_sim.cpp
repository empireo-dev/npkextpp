// tune_sim: runs the real pipeline and tuner against a simulated disk
// (docs/simulated_disk.md). Nothing is written.
//
//   tune_sim --install DIR [--in NAME...] --disk ssd|hdd [--set name=value ...]
//            [--writers N] [--repeat N] [--decoders N]
//   tune_sim --disk ssd|hdd --curve --file-size BYTES [--seconds S] [--set ...]
#include "../sim/simulated_disk.hpp"

#include "npk/inputs.hpp"
#include "npk/install.hpp"
#include "npk/names.hpp"
#include "npk/pipeline/pipeline.hpp"
#include "npk/plan.hpp"

#include <CLI11.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Writes go to one simulated disk, reads to another (or to the same one, or nowhere).
class SplitDisk final : public npk::pipeline::FakeDisk {
public:
    SplitDisk(npk::pipeline::FakeDisk& output, npk::pipeline::FakeDisk* input) : output_(output), input_(input) {}
    void write_file(unsigned writer, std::size_t bytes) override { output_.write_file(writer, bytes); }
    void read(std::size_t bytes) override
    {
        if (input_) input_->read(bytes);
    }

private:
    npk::pipeline::FakeDisk& output_;
    npk::pipeline::FakeDisk* input_;
};

constexpr double kFileCost = 64 * 1024;   // as in the tuner's score

double since(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

int curve(const npk::sim::DiskParams& params, std::size_t file_size, double seconds)
{
    std::printf("file size %zu bytes, %.0f s per count\n", file_size, seconds);
    std::printf("%8s %12s %12s %12s\n", "writers", "MB/s", "files/s", "score");
    for (const unsigned writers : {1u, 2u, 4u, 8u, 16u, 32u}) {
        npk::sim::SimulatedDisk disk(params);
        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> files{0};
        const auto start = std::chrono::steady_clock::now();
        {
            std::vector<std::jthread> threads;
            for (unsigned i = 0; i < writers; ++i) {
                threads.emplace_back([&, i] {
                    while (!stop.load(std::memory_order_relaxed)) {
                        disk.write_file(i, file_size);
                        files.fetch_add(1, std::memory_order_relaxed);
                    }
                });
            }
            std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
            stop = true;
        }
        const double elapsed = since(start);
        const double n = static_cast<double>(files.load()) / elapsed;
        const double bytes = n * static_cast<double>(file_size);
        std::printf("%8u %12.1f %12.0f %12.1f\n", writers, bytes / 1e6, n, (bytes + kFileCost * n) / 1e6);
    }
    return 0;
}

// Reads every archive once so later passes come from the OS cache.
void warm_cache(const npk::Plan& plan)
{
    std::vector<char> buffer(std::size_t{8} << 20);
    for (const auto& group : plan.groups) {
        for (const auto& source : group.sources) {
            std::ifstream in(source.path, std::ios::binary);
            while (in.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || in.gcount() > 0) {
            }
        }
    }
}

// Drops the archives from the OS cache, so a real run reads them from the disk.
void drop_cache(const npk::Plan& plan)
{
    ::sync();
    for (const auto& group : plan.groups) {
        for (const auto& source : group.sources) {
            const int fd = ::open(source.path.c_str(), O_RDONLY);
            if (fd < 0) continue;
            ::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
            ::close(fd);
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    CLI::App app{"Runs the pipeline and tuner against a simulated disk"};
    std::string disk_name = "ssd";
    std::vector<std::string> settings, inputs;
    std::optional<fs::path> install;
    unsigned writers = 0, repeat = 1, decoders = 0;
    bool curve_mode = false;
    std::size_t file_size = 64 * 1024;
    double seconds = 5;
    app.add_option("--disk", disk_name, "ssd or hdd")->check(CLI::IsMember({"ssd", "hdd"}));
    app.add_option("--set", settings, "change a disk parameter: name=value");
    app.add_option("--install", install, "game installation");
    app.add_option("--in", inputs, "archives (names or paths) instead of all");
    std::optional<fs::path> real;
    app.add_option("--real", real, "write real files into this folder (emptied first) instead of a simulated disk;"
                                   " the archives are read cold");
    std::string kind_name;
    app.add_option("--kind", kind_name, "tune as for this kind of disk (default: the --disk preset; with --real: probe)")
        ->check(CLI::IsMember({"ssd", "hdd"}));
    app.add_option("--writers", writers, "fixed writer count; no tuner");
    app.add_option("--repeat", repeat, "go through the archives this many times")->check(CLI::PositiveNumber);
    app.add_option("--decoders", decoders, "decoder threads (default: one per core)");
    app.add_flag("--curve", curve_mode, "measure the disk alone for 1..32 writers");
    app.add_option("--file-size", file_size, "file size for --curve");
    app.add_option("--seconds", seconds, "seconds per writer count for --curve");
    std::string input_name = "same";
    std::vector<std::string> input_settings;
    app.add_option("--input", input_name, "where the archives are: same (the output disk), ram, ssd or hdd")
        ->check(CLI::IsMember({"same", "ram", "ssd", "hdd"}));
    app.add_option("--input-set", input_settings, "change an input disk parameter: name=value");
    std::vector<unsigned> schedule;
    double period = 10;
    bool trace = false;
    app.add_option("--schedule", schedule, "cycle through these writer counts instead of tuning");
    app.add_option("--period", period, "seconds per --schedule step");
    app.add_flag("--trace", trace, "print the tuner's raw readings every 0.5 s");
    CLI11_PARSE(app, argc, argv);

    npk::sim::DiskParams params = disk_name == "hdd" ? npk::sim::hdd() : npk::sim::ssd();
    for (const std::string& setting : settings) {
        if (const std::string error = npk::sim::set_param(params, setting); !error.empty()) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 2;
        }
    }
    std::printf("disk %s: %s\n", disk_name.c_str(), npk::sim::describe(params).c_str());
    if (curve_mode) return curve(params, file_size, seconds);

    if (!install) {
        std::fprintf(stderr, "--install DIR is required (or use --curve)\n");
        return 2;
    }
    auto installation = npk::Installation::open(*install, npk::ArchiveSearch::scan);
    if (!installation) {
        std::fprintf(stderr, "%s\n", installation.error().message.c_str());
        return 2;
    }
    auto manifest = npk::ResourceManifest::from_files(installation->resdb_files());
    if (!manifest) {
        std::fprintf(stderr, "%s\n", manifest.error().message.c_str());
        return 2;
    }
    std::vector<fs::path> archives = installation->archives();
    if (!inputs.empty()) {
        auto resolved = npk::resolve_inputs(inputs, &*installation);
        if (!resolved) {
            std::fprintf(stderr, "%s\n", resolved.error().message.c_str());
            return 2;
        }
        archives = std::move(*resolved);
    }
    npk::Planner planner(*manifest, &*installation);
    npk::Plan plan = planner.build(archives, {});
    if (real) {
        if (repeat != 1) {
            std::fprintf(stderr, "--real writes one pass only\n");
            return 2;
        }
        fs::remove_all(*real);
        drop_cache(plan);
    } else {
        warm_cache(plan);
    }

    // Each pass writes the same files again; paths don't matter to a fake disk.
    const std::vector<npk::OutputGroup> one_pass = plan.groups;
    for (unsigned i = 1; i < repeat; ++i) plan.groups.insert(plan.groups.end(), one_pass.begin(), one_pass.end());

    npk::sim::SimulatedDisk output_disk(params);
    std::unique_ptr<npk::sim::SimulatedDisk> input_disk;
    if (input_name == "ssd" || input_name == "hdd") {
        npk::sim::DiskParams input = input_name == "hdd" ? npk::sim::hdd() : npk::sim::ssd();
        input.shared_with_input = true;   // its reads cost time
        input.seed = params.seed + 1;
        for (const std::string& setting : input_settings) {
            if (const std::string error = npk::sim::set_param(input, setting); !error.empty()) {
                std::fprintf(stderr, "%s\n", error.c_str());
                return 2;
            }
        }
        std::printf("input %s: %s\n", input_name.c_str(), npk::sim::describe(input).c_str());
        input_disk = std::make_unique<npk::sim::SimulatedDisk>(input);
    }
    npk::pipeline::FakeDisk* reads = input_name == "same" ? &output_disk : input_disk.get();
    if (input_name == "ram") reads = nullptr;
    SplitDisk disk(output_disk, reads);
    npk::pipeline::Options options;
    options.output_root = real ? *real : fs::path("unused");
    options.write_index = false;
    if (!real) options.fake_disk = &disk;
    if (!kind_name.empty() || !real) {
        const std::string kind = kind_name.empty() ? disk_name : kind_name;
        options.disk_kind = kind == "hdd" ? npk::pipeline::DiskKind::hdd : npk::pipeline::DiskKind::ssd;
    }
    options.decoder_threads = decoders;
    if (writers) {
        options.adaptive = false;
        options.initial_writers = writers;
    }
    if (!schedule.empty()) {
        options.adaptive = false;
        options.writer_schedule = schedule;
        options.schedule_period = period;
        options.initial_writers = schedule.front();
    }
    options.trace = trace;
    const auto start = std::chrono::steady_clock::now();
    std::mutex print_mutex;
    options.log = [&](std::string_view line) {
        std::lock_guard lock(print_mutex);
        std::printf("%7.1f s  %.*s\n", since(start), static_cast<int>(line.size()), line.data());
        std::fflush(stdout);
    };

    npk::pipeline::Progress progress;
    std::atomic<bool> done{false};
    std::jthread reporter([&] {
        std::uint64_t bytes = 0, files = 0;
        double last = 0;
        while (!done && !trace) {
            for (int i = 0; i < 50 && !done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const double now = since(start);
            const std::uint64_t b = progress.bytes_written.load(), f = progress.files_written.load();
            const double dt = now - last;
            std::lock_guard lock(print_mutex);
            std::printf("%7.1f s  %8.1f MB/s %8.0f files/s  %u writers\n", now,
                        static_cast<double>(b - bytes) / dt / 1e6, static_cast<double>(f - files) / dt,
                        progress.writers_active.load());
            std::fflush(stdout);
            bytes = b, files = f, last = now;
        }
    });
    npk::pipeline::Summary summary = npk::pipeline::run(plan, options, progress);
    if (real) {
        ::sync();   // count writing the data out
        summary.seconds = since(start);
    }
    done = true;
    reporter.join();

    const double mb = static_cast<double>(summary.bytes_written) / 1e6;
    std::printf("result: %.1f s, %.1f MB/s, %.0f files/s, %llu files, %u writers at the end, %zu MiB reads, tuned as %s\n",
                summary.seconds, mb / summary.seconds, static_cast<double>(summary.files_written) / summary.seconds,
                static_cast<unsigned long long>(summary.files_written), summary.writers, summary.read_block >> 20,
                npk::pipeline::name_of(summary.disk));
    return summary.files_failed ? 1 : 0;
}
