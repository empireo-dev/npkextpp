#pragma once

#include "npk/pipeline/byte_budget.hpp"
#include "npk/plan.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace npk::pipeline {

struct ChunkEntry {
    PlannedEntry* planned = nullptr;
    const Entry* entry = nullptr;
    bool shared = false;   // its stored bytes overlap another entry's in this chunk: decode a copy
};

// One large sequential read covering several neighbouring entries of one
// archive. Shared (shared_ptr) by every DecodeTask and stored-mode WriteJob that
// points into it; its read-budget bytes go back when the last one lets go.
struct ReadChunk {
    BudgetLease lease;   // first member: destroyed last, after the buffer is freed
    const ArchiveSource* source = nullptr;
    std::uint32_t source_index = 0;   // position in plan order
    std::uint64_t file_offset = 0;
    std::size_t size = 0;
    std::unique_ptr<std::byte[]> data;
    std::vector<ChunkEntry> entries;   // in storage order

    [[nodiscard]] std::span<std::byte> stored(const Entry& entry) noexcept
    {
        return {data.get() + (entry.offset - file_offset), entry.packed_size};
    }
};

// A run of a chunk's entries (~1 MiB of stored data) for one decoder.
struct DecodeTask {
    std::shared_ptr<ReadChunk> chunk;
    std::uint32_t first = 0;   // index into chunk->entries
    std::uint32_t count = 0;
};

// A finished file waiting to be written.
struct WriteJob {
    BudgetLease lease;                    // output-budget bytes of `owned`; destroyed last
    PlannedEntry* planned = nullptr;
    std::uint32_t source_index = 0;       // the archive's position in plan order
    std::string path;                     // final path relative to the output folder
    std::span<const std::byte> data;      // into `owned` or into `chunk`
    std::unique_ptr<std::byte[]> owned;   // decoded output
    std::shared_ptr<ReadChunk> chunk;     // set for stored entries written straight from the chunk
};

} // namespace npk::pipeline
