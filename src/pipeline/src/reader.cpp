#include "stages.hpp"

#include "npk/input_file.hpp"
#include "npk/paths.hpp"
#include "npk/read_range.hpp"

#include <algorithm>

namespace npk::pipeline {

namespace {

// Entries whose stored bytes overlap another entry's in the same chunk must be
// decoded from a copy: decryption works in place. Entries are in offset order.
void mark_shared(std::vector<ChunkEntry>& entries)
{
    for (std::size_t a = 0; a < entries.size(); ++a) {
        const Entry& ea = *entries[a].entry;
        if (ea.packed_size == 0) continue;
        for (std::size_t b = a + 1; b < entries.size(); ++b) {
            const Entry& eb = *entries[b].entry;
            if (eb.offset >= stored_end(ea)) break;   // sorted by offset: nothing further can overlap a
            if (eb.packed_size == 0) continue;
            entries[a].shared = true;
            entries[b].shared = true;
        }
    }
}

} // namespace

void Reader::run()
{
    clock_.enter(StageState::working);
    const std::stop_token stop = ctx_.stop.get_token();
    std::uint32_t index = 0;
    for (OutputGroup& group : ctx_.plan.groups) {
        for (ArchiveSource& source : group.sources) {
            if (stop.stop_requested()) break;
            read_source(source, index++);
        }
    }
    ctx_.decode_queue.close();
    clock_.enter(StageState::waiting_input);
    ctx_.progress.reading_done.store(true, std::memory_order_relaxed);
}

void Reader::read_source(ArchiveSource& source, std::uint32_t index)
{
    // The entries to write, in storage order (the plan keeps them sorted by offset).
    std::vector<PlannedEntry*> todo;
    for (PlannedEntry& planned : source.entries) {
        if (planned.disposition != Disposition::skip && planned.status == EntryStatus::pending) todo.push_back(&planned);
    }
    if (todo.empty()) return;

    ctx_.progress.current_source.store(&source, std::memory_order_relaxed);
    ctx_.progress.archives_started.fetch_add(1, std::memory_order_relaxed);

    auto file = InputFile::open(source.path);
    if (!file) {
        for (PlannedEntry* planned : todo) ctx_.fail(*planned, file.error().message, index);
        return;
    }
    file->advise_sequential();

    // Entries whose bytes are not inside the file fail on their own.
    std::erase_if(todo, [&](PlannedEntry* planned) {
        if (stored_end(source.entry(*planned)) <= file->size()) return false;
        ctx_.fail(*planned, "data runs past the end of " + to_utf8(source.path.filename()), index);
        return true;
    });

    const std::stop_token stop = ctx_.stop.get_token();
    std::size_t i = 0;
    while (i < todo.size() && !stop.stop_requested()) {
        // Merge neighbours into one read of at most read_block bytes.
        const std::uint64_t block = ctx_.tunables.read_block.load(std::memory_order_relaxed);
        const auto [start, length, j] = next_read_range(i, todo.size(),
            [&](std::size_t index) -> const Entry& { return source.entry(*todo[index]); },
            block, kMaxGap);
        const auto size = static_cast<std::size_t>(length);

        clock_.enter(StageState::waiting_output);
        if (!ctx_.read_budget.acquire(size, stop)) return;
        clock_.enter(StageState::working);

        auto chunk = std::make_shared<ReadChunk>();
        chunk->lease = BudgetLease(ctx_.read_budget, size);
        chunk->source = &source;
        chunk->source_index = index;
        chunk->file_offset = start;
        chunk->size = size;
        chunk->data = std::make_unique_for_overwrite<std::byte[]>(size);
        chunk->entries.reserve(j - i);
        for (std::size_t k = i; k < j; ++k) chunk->entries.push_back({todo[k], &source.entry(*todo[k]), false});
        i = j;

        if (auto read = file->read_at(start, {chunk->data.get(), size}); !read) {
            for (const ChunkEntry& entry : chunk->entries) ctx_.fail(*entry.planned, read.error().message, index);
            continue;
        }
        if (FakeDisk* disk = ctx_.options.fake_disk) {
            disk->read(size);   // keep the pages cached: the next run reads them from RAM
        } else {
            file->forget(start, size);   // the bytes are in our buffer now; never read again
        }
        ctx_.progress.bytes_read.fetch_add(size, std::memory_order_relaxed);
        ctx_.progress.blocks_read.fetch_add(1, std::memory_order_relaxed);

        mark_shared(chunk->entries);
        if (!emit(std::move(chunk))) return;
    }
}

// Cuts a chunk into tasks of about kTaskBytes of stored data.
bool Reader::emit(std::shared_ptr<ReadChunk> chunk)
{
    const std::stop_token stop = ctx_.stop.get_token();
    const auto count = static_cast<std::uint32_t>(chunk->entries.size());
    std::uint32_t first = 0;
    while (first < count) {
        std::uint32_t last = first;
        std::uint64_t bytes = 0;
        while (last < count && (last == first || bytes < kTaskBytes)) {
            bytes += chunk->entries[last].entry->packed_size;
            ++last;
        }
        clock_.enter(StageState::waiting_output);
        const bool pushed = ctx_.decode_queue.push(DecodeTask{chunk, first, last - first}, stop);
        clock_.enter(StageState::working);
        if (!pushed) return false;
        first = last;
    }
    return true;
}

} // namespace npk::pipeline
