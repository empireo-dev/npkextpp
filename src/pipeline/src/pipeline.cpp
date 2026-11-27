#include "npk/pipeline/pipeline.hpp"

#include "npk/paths.hpp"
#include "npk/pipeline/index_writer.hpp"
#include "npk/pipeline/disk_probe.hpp"
#include "stages.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <system_error>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace npk::pipeline {

namespace {

constexpr std::size_t kMiB = std::size_t{1} << 20;
constexpr std::size_t kMaxDefaultBudget = 1024 * kMiB;
constexpr std::size_t kMinBudget = 64 * kMiB;

std::uint64_t physical_memory()
{
#if defined(_WIN32)
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof status;
    return ::GlobalMemoryStatusEx(&status) ? status.ullTotalPhys : 0;
#elif defined(_SC_PHYS_PAGES) && defined(_SC_PAGE_SIZE)
    const long pages = ::sysconf(_SC_PHYS_PAGES);
    const long page = ::sysconf(_SC_PAGE_SIZE);
    return pages > 0 && page > 0 ? static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(page) : 0;
#else
    return 0;
#endif
}

std::size_t default_budget()
{
    const std::uint64_t ram = physical_memory();
    if (ram == 0) return kMaxDefaultBudget / 2;
    return static_cast<std::size_t>(std::clamp<std::uint64_t>(ram / 8, kMinBudget, kMaxDefaultBudget));
}

// The writeback tracker keeps recently written files open.
void raise_open_file_limit()
{
#if !defined(_WIN32)
    rlimit limit{};
    if (::getrlimit(RLIMIT_NOFILE, &limit) != 0) return;
    const rlim_t wanted = std::min<rlim_t>(limit.rlim_max, 65536);
    if (limit.rlim_cur < wanted) {
        limit.rlim_cur = wanted;
        (void)::setrlimit(RLIMIT_NOFILE, &limit);
    }
#endif
}

fs::path os_path(const fs::path& root, const std::string& relative)
{
    fs::path path = root / from_utf8(relative);
#if defined(_WIN32)
    path.make_preferred();
#endif
    return path;
}

std::optional<Error> create_folders(Context& ctx)
{
    std::error_code error;
    fs::create_directories(ctx.options.output_root, error);
    if (error || !fs::is_directory(ctx.options.output_root, error)) {
        return Error{Errc::io_error, "cannot create the output folder " + to_utf8(ctx.options.output_root) +
                                         (error ? ": " + error.message() : "")};
    }
    for (const std::string& folder : ctx.plan.directories) {
        fs::create_directories(os_path(ctx.output_root, folder), error);
        if (!error) continue;
        if (error == std::errc::no_space_on_device) return Error{Errc::no_space, "cannot create " + folder + ": " + error.message()};
        if (error == std::errc::read_only_file_system) return Error{Errc::read_only, "cannot create " + folder + ": " + error.message()};
        // Anything else (a bad name, say) only affects the files in that folder: their writes fail.
    }
    return std::nullopt;
}

} // namespace

Summary run(Plan& plan, const Options& options, Progress& progress, std::stop_token cancel)
{
    const auto started = std::chrono::steady_clock::now();
    Summary summary;

    const std::size_t budget = options.memory_budget ? options.memory_budget : default_budget();
    const std::size_t read_bytes = std::max<std::size_t>(budget / 3, 1);
    const std::size_t output_bytes = std::max<std::size_t>(budget - read_bytes, 1);
    Context ctx(plan, options, progress, read_bytes, output_bytes);
    ctx.output_root = long_path(options.output_root);
    std::stop_callback forward_cancel(cancel, [&ctx] { ctx.stop.request_stop(); });

    progress.read_block_kib.store(static_cast<std::uint32_t>(ctx.tunables.read_block.load() >> 10));

    if (auto error = options.fake_disk ? std::nullopt : create_folders(ctx)) {
        summary.fatal = std::move(error);
    } else {
        // What kind of disk the output is on decides how the tuner searches.
        if (options.disk_kind) {
            ctx.disk = *options.disk_kind;
        } else if (options.adaptive && !options.fake_disk) {
            std::vector<fs::path> archives;
            for (const OutputGroup& group : plan.groups) {
                for (const ArchiveSource& source : group.sources) archives.push_back(source.path);
            }
            const DiskProbe probe = probe_disk(ctx.output_root, archives);
            ctx.disk = probe.kind;
            summary.probe_read_ms = probe.slow_read_ms;
            if (probe.kind == DiskKind::unknown) {
                ctx.log("disk: " + probe.problem + "; tuning as for an SSD");
            } else {
                char line[200];
                std::snprintf(line, sizeof line, "disk: %s (%zu random reads from %s: median %.2f ms, 90th percentile %.2f ms)",
                              name_of(probe.kind), probe.reads, probe.read_from.c_str(), probe.median_read_ms,
                              probe.slow_read_ms);
                ctx.log(line);
            }
        }
        summary.disk = ctx.disk;
        ctx.profile = profile_for(ctx.disk);
        const unsigned writers =
            std::clamp(options.initial_writers ? options.initial_writers : ctx.profile.initial, 1u, Tunables::kMaxWriters);
        ctx.tunables.writers_allowed.store(writers);
        progress.writers_active.store(writers);

        raise_open_file_limit();
        const unsigned hardware = std::max(std::thread::hardware_concurrency(), 1u);
        const unsigned decoders = options.decoder_threads ? options.decoder_threads : hardware;
        ctx.log("pipeline: " + std::to_string(decoders) + " decoders, " + std::to_string(writers) +
                " writers to start, memory budget " + std::to_string(budget / kMiB) + " MiB");

        Reader reader(ctx);
        DecoderPool decoder_pool(ctx);
        WriterPool writer_pool(ctx);
        Tuner tuner(ctx, reader, decoder_pool, writer_pool);

        writer_pool.start();
        decoder_pool.start(decoders);
        std::stop_source tuner_stop;
        std::stop_callback stop_tuner(ctx.stop.get_token(), [&tuner_stop] { tuner_stop.request_stop(); });
        std::jthread tuner_thread([&] { tuner.run(tuner_stop.get_token()); });   // also publishes the stage loads

        std::jthread reader_thread([&reader] { reader.run(); });
        reader_thread.join();
        decoder_pool.join();   // the last decoder closes the write queue
        writer_pool.join();
        tuner_stop.request_stop();
        if (tuner_thread.joinable()) tuner_thread.join();
        ctx.writeback.drain();
        progress.bytes_on_disk.store(ctx.writeback.bytes_on_disk());
        summary.fatal = ctx.fatal.get();
    }

    for (const OutputGroup& group : plan.groups) {
        for (const ArchiveSource& source : group.sources) {
            for (const PlannedEntry& planned : source.entries) {
                if (planned.disposition == Disposition::skip) continue;
                switch (planned.status) {
                case EntryStatus::written: ++summary.files_written; break;
                case EntryStatus::failed: ++summary.files_failed; break;
                case EntryStatus::pending: ++summary.files_not_reached; break;
                case EntryStatus::existing: ++summary.files_existing; break;
                }
            }
        }
    }
    summary.bytes_written = progress.bytes_written.load();
    summary.cancelled = cancel.stop_requested();
    summary.writers = ctx.tunables.writers_allowed.load();
    summary.read_block = ctx.tunables.read_block.load();

    if (options.write_index) {
        if (auto written = write_index(plan, options.output_root); !written) summary.index_error = written.error();
    }
    summary.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return summary;
}

} // namespace npk::pipeline
