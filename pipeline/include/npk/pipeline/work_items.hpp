#pragma once

#include "npk/pipeline/byte_budget.hpp"
#include "npk/plan.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace npk::pipeline {

// One large sequential read covering several neighbouring entries of one
// archive. Shared by every DecodeTask (and stored-mode WriteJob) that points
// into it; its read-budget bytes are returned when the last one lets go.
struct ReadChunk {
    ArchiveSource* source = nullptr;
    std::uint64_t file_offset = 0;
    std::unique_ptr<std::byte[]> data;
    std::size_t size = 0;
    BudgetLease lease;

    [[nodiscard]] std::span<std::byte> bytes() noexcept { return {data.get(), size}; }
};

// A slice of a chunk's entries (~1 MB of stored data) for one decoder.
struct DecodeTask {
    std::shared_ptr<ReadChunk> chunk;
    std::uint32_t first = 0;   // index into source->entries
    std::uint32_t count = 0;
};

// A finished file waiting to be written.
struct WriteJob {
    PlannedEntry* entry = nullptr;
    const ArchiveSource* source = nullptr;
    std::span<const std::byte> data;      // into `owned` or into `chunk`
    std::unique_ptr<std::byte[]> owned;   // decompressed output
    BudgetLease lease;                    // output-budget bytes of `owned`
    std::shared_ptr<ReadChunk> chunk;     // set for stored (mode 0) entries
};

} // namespace npk::pipeline
