#include "stages.hpp"

#include "npk/paths.hpp"

#include <system_error>

namespace fs = std::filesystem;

namespace npk::pipeline {

void WriterPool::start()
{
    for (unsigned i = 0; i < Tunables::kMaxWriters; ++i) {
        StageClock& clock = clocks_.emplace_back();
        clock.enter(StageState::parked);
        threads_.emplace_back([this, i, &clock] { run(i, clock); });
    }
}

void WriterPool::allowed_changed()
{
    { std::lock_guard lock(park_mutex_); }   // a writer between its check and its wait sees the new value
    park_.notify_all();
}

void WriterPool::join()
{
    allowed_changed();   // parked writers re-check: the queue is closed
    for (auto& thread : threads_) {
        if (thread.joinable()) thread.join();
    }
    threads_.clear();
}

void WriterPool::run(unsigned index, StageClock& clock)
{
    const std::stop_token stop = ctx_.stop.get_token();
    const auto active = [&] { return index < ctx_.tunables.writers_allowed.load(std::memory_order_relaxed); };

    while (!stop.stop_requested()) {
        if (!active()) {
            clock.enter(StageState::parked);
            std::unique_lock lock(park_mutex_);
            park_.wait(lock, stop, [&] { return active() || ctx_.write_queue.closed(); });
            if (!active()) break;   // stopped, or the queue closed while parked: the active writers drain it
            continue;
        }
        clock.enter(StageState::waiting_input);
        std::optional<WriteJob> job = ctx_.write_queue.pop(stop);
        if (!job) break;
        clock.enter(StageState::working);
        write(index, *job);
    }
    clock.enter(StageState::parked);
}

void WriterPool::fail_write(WriteJob& job, const fs::path& path, Error error)
{
    std::error_code ignored;
    fs::remove(path, ignored);   // no half-written files
    if (is_fatal(error)) ctx_.fatal.report(error, ctx_.stop);
    ctx_.fail(*job.planned, "cannot write " + job.path + ": " + error.message, job.source_index);
}

void WriterPool::write(unsigned index, WriteJob& job)
{
    if (FakeDisk* disk = ctx_.options.fake_disk) {
        disk->write_file(index, job.data.size());
        return written(job);
    }

    fs::path path = ctx_.output_root / from_utf8(job.path);
#if defined(_WIN32)
    path.make_preferred();   // extended-length paths take backslashes only
#endif

    auto file = OutputFile::create(path);
    if (!file && file.error().code == Errc::too_many_open_files) {
        ctx_.writeback.finish_oldest(64);
        file = OutputFile::create(path);
    }
    if (!file && file.error().code == Errc::not_found) {   // folder missing (removed while we run?)
        std::error_code ignored;
        fs::create_directories(path.parent_path(), ignored);
        file = OutputFile::create(path);
    }
    if (!file) return fail_write(job, path, std::move(file.error()));

    if (auto written = file->write(job.data); !written) {
        (void)file->close();
        return fail_write(job, path, std::move(written.error()));
    }
    file->start_writeback();
    ctx_.writeback.submit(std::move(*file), job.data.size());
    written(job);
}

void WriterPool::written(WriteJob& job)
{
    PlannedEntry& planned = *job.planned;
    if (planned.unnamed) planned.path = job.path;   // now with its extension, for the index
    planned.status = EntryStatus::written;
    ctx_.progress.files_written.fetch_add(1, std::memory_order_relaxed);
    ctx_.progress.bytes_written.fetch_add(job.data.size(), std::memory_order_relaxed);
    ctx_.progress.bytes_on_disk.store(ctx_.writeback.bytes_on_disk(), std::memory_order_relaxed);
    if (SourceProgress* counts = ctx_.source_progress(job.source_index)) {
        counts->bytes_done.fetch_add(job.data.size(), std::memory_order_relaxed);
        counts->files_done.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace npk::pipeline
