#pragma once

#include "npk/archive.hpp"
#include "npk/error.hpp"
#include "npk/install.hpp"
#include "npk/names.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace npk {

// What happened to one planned entry. Each slot is written by exactly one
// thread, so results need no locking.
enum class EntryStatus : std::uint8_t {
    pending,
    written,
    overridden,        // a higher-priority archive provides this file
    failed_read,
    failed_decode,     // crc / decompression / size error
    failed_write,
};

struct PlannedEntry {
    std::uint32_t entry_index = 0;   // index into Archive::entries()
    std::string output_path;         // relative to the output root, '/' separators
    bool write = true;               // false: recorded in the index only
    EntryStatus status = EntryStatus::pending;
};

// One archive to unpack, entries sorted by offset for front-to-back reading.
struct ArchiveJob {
    Archive archive;
    std::string mount_prefix;
    std::vector<PlannedEntry> entries;
};

struct Plan {
    std::filesystem::path output_root;
    std::vector<ArchiveJob> archives;
    std::vector<std::string> directories;   // created before extraction starts
    std::uint64_t total_files = 0;
    std::uint64_t total_raw_bytes = 0;
};

struct PlanOptions {
    std::filesystem::path output_root;
    std::vector<std::string> only;   // archive names / substrings; empty = all
    bool keep_overridden = false;    // also write lower-priority copies
};

// Turns an installation into a Plan: which entry of which archive goes where.
class Planner {
public:
    Planner(const Installation& installation, NameResolver& names);

    [[nodiscard]] Result<Plan> build(const PlanOptions& options);

private:
    const Installation& installation_;
    NameResolver& names_;
};

} // namespace npk
