#pragma once

// State shared by the pipeline stages (private to npk_pipeline).

#include "npk/pipeline/bounded_queue.hpp"
#include "npk/pipeline/byte_budget.hpp"
#include "npk/pipeline/pipeline.hpp"
#include "npk/pipeline/progress.hpp"
#include "npk/pipeline/writeback.hpp"
#include "tunables.hpp"
#include "work_items.hpp"

#include <filesystem>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>

namespace npk::pipeline {

// The first fatal error, which also stops the pipeline.
class FatalSlot {
public:
    void report(Error error, std::stop_source& stop)
    {
        {
            std::lock_guard lock(mutex_);
            if (!error_) error_ = std::move(error);
        }
        stop.request_stop();
    }
    [[nodiscard]] std::optional<Error> get() const
    {
        std::lock_guard lock(mutex_);
        return error_;
    }

private:
    mutable std::mutex mutex_;
    std::optional<Error> error_;
};

struct Context {
    static constexpr std::size_t kDecodeQueueTasks = 4096;
    static constexpr std::size_t kWriteQueueJobs = 8192;

    Context(Plan& plan_, const Options& options_, Progress& progress_, std::size_t read_bytes, std::size_t output_bytes)
        : plan(plan_)
        , options(options_)
        , progress(progress_)
        , read_budget(read_bytes)
        , output_budget(output_bytes)
    {
    }

    Plan& plan;
    const Options& options;
    Progress& progress;
    std::filesystem::path output_root;   // in the OS's form (extended-length on Windows)

    Tunables tunables;
    DiskKind disk = DiskKind::unknown;
    WriterProfile profile = kSsdProfile;
    std::stop_source stop;
    FatalSlot fatal;

    // Budgets before the queues: queued items hold leases on them.
    ByteBudget read_budget;
    ByteBudget output_budget;
    BoundedQueue<DecodeTask> decode_queue{kDecodeQueueTasks};
    BoundedQueue<WriteJob> write_queue{kWriteQueueJobs};
    WritebackTracker writeback;

    // `source`: the archive's position in plan order (for Progress::sources()).
    void fail(PlannedEntry& entry, std::string message, std::uint32_t source)
    {
        entry.status = EntryStatus::failed;
        entry.error = std::move(message);
        progress.files_failed.fetch_add(1, std::memory_order_relaxed);
        if (SourceProgress* counts = source_progress(source)) {
            counts->files_failed.fetch_add(1, std::memory_order_relaxed);
            counts->files_done.fetch_add(1, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] SourceProgress* source_progress(std::uint32_t source) noexcept
    {
        const auto all = progress.sources();
        return source < all.size() ? &all[source] : nullptr;
    }

    void log(std::string_view line) const
    {
        if (options.log) options.log(line);
    }
};

} // namespace npk::pipeline
