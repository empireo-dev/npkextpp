#include "stages.hpp"

#include "npk/decoder.hpp"
#include "npk/hash.hpp"

#include <algorithm>
#include <string>

namespace npk::pipeline {

void DecoderPool::start(unsigned threads)
{
    threads = std::max(threads, 1u);
    running_.store(threads);
    for (unsigned i = 0; i < threads; ++i) {
        StageClock& clock = clocks_.emplace_back();
        threads_.emplace_back([this, &clock] { run(clock); });
    }
}

void DecoderPool::join()
{
    for (auto& thread : threads_) {
        if (thread.joinable()) thread.join();
    }
    threads_.clear();
}

void DecoderPool::run(StageClock& clock)
{
    const std::stop_token stop = ctx_.stop.get_token();
    const bool verify = ctx_.options.verify;
    Decoder decoder;
    std::vector<std::byte> scratch;

    for (;;) {
        clock.enter(StageState::waiting_input);
        std::optional<DecodeTask> task = ctx_.decode_queue.pop(stop);
        if (!task) break;
        clock.enter(StageState::working);

        ReadChunk& chunk = *task->chunk;
        bool stopping = false;
        for (std::uint32_t k = task->first; k < task->first + task->count && !stopping; ++k) {
            ChunkEntry& item = chunk.entries[k];
            const Entry& entry = *item.entry;
            PlannedEntry& planned = *item.planned;
            std::span<std::byte> stored = chunk.stored(entry);

            WriteJob job;
            job.planned = &planned;
            job.source_index = chunk.source_index;

            if (entry.compression() == Compression::none && entry.raw_size > 0 && !item.shared) {
                // Stored as is: check and decrypt in place, write straight from the chunk.
                if (verify && murmur3_32(stored) != entry.crc_packed) {
                    ctx_.fail(planned, "stored data is damaged (crc_packed does not match)", chunk.source_index);
                    continue;
                }
                if (entry.encrypted()) decrypt(stored);
                if (stored.size() != entry.raw_size) {
                    ctx_.fail(planned, "decoded " + std::to_string(stored.size()) + " bytes, expected " +
                                           std::to_string(entry.raw_size), chunk.source_index);
                    continue;
                }
                if (verify && murmur3_32(stored) != entry.crc_raw) {
                    ctx_.fail(planned, "decoded data is wrong (crc_raw does not match)", chunk.source_index);
                    continue;
                }
                job.data = stored;
                job.chunk = task->chunk;
            } else {
                if (item.shared) {   // overlaps another entry: work on a private copy
                    scratch.assign(stored.begin(), stored.end());
                    stored = scratch;
                }
                clock.enter(StageState::waiting_output);
                const bool granted = ctx_.output_budget.acquire(entry.raw_size, stop);
                clock.enter(StageState::working);
                if (!granted) {
                    stopping = true;
                    break;
                }
                job.lease = BudgetLease(ctx_.output_budget, entry.raw_size);
                job.owned = std::make_unique_for_overwrite<std::byte[]>(entry.raw_size);
                const std::span<std::byte> out(job.owned.get(), entry.raw_size);
                if (auto decoded = decoder.decode(stored, entry, out, verify); !decoded) {
                    ctx_.fail(planned, decoded.error().message, chunk.source_index);
                    continue;   // the job (and its budget) goes away here
                }
                job.data = out;
            }

            job.path = output_path(planned, job.data);
            ctx_.progress.files_decoded.fetch_add(1, std::memory_order_relaxed);
            clock.enter(StageState::waiting_output);
            const bool pushed = ctx_.write_queue.push(std::move(job), stop);
            clock.enter(StageState::working);
            if (!pushed) stopping = true;
        }
        if (stopping) break;
    }

    clock.enter(StageState::waiting_input);
    if (running_.fetch_sub(1) == 1) {   // the last decoder out closes the queue
        ctx_.write_queue.close();
        ctx_.progress.decoding_done.store(true, std::memory_order_relaxed);
    }
}

} // namespace npk::pipeline
