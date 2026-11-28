#pragma once

#include "npk/entry.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace npk {

struct ReadRange {
    std::uint64_t offset;
    std::uint64_t size;
    std::size_t next;
};

inline std::uint64_t stored_end(const Entry& entry) noexcept
{
    return std::uint64_t{entry.offset} + entry.packed_size;
}

// Entries are in offset order. A single oversized entry gets its own range.
template <class EntryAt>
ReadRange next_read_range(std::size_t first, std::size_t count, EntryAt entry_at,
                          std::uint64_t max_size, std::uint64_t max_gap)
{
    const std::uint64_t start = entry_at(first).offset;
    auto end = stored_end(entry_at(first));
    auto next = first + 1;
    for (; next < count; ++next) {
        const Entry& entry = entry_at(next);
        const auto gap = entry.offset > end ? entry.offset - end : 0;
        const auto new_end = std::max(end, stored_end(entry));
        if (gap > max_gap || new_end - start > max_size) break;
        end = new_end;
    }
    return {start, end - start, next};
}

} // namespace npk
