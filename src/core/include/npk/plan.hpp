#pragma once

#include "npk/entry.hpp"
#include "npk/install.hpp"
#include "npk/names.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace npk {

// Output layout (all paths relative to the output folder, '/' separated):
//
//   res/<path in the tree>                 files whose name was recovered
//   res/ui/_unnamed/<id>.<ext>             files whose name is unknown, in their archive's folder
//   res/ui/_overridden/<source>/<path>     lower-priority copies (keep_overridden only)
//   script/...                             the script tree
//   <stem>/...                             an archive outside every mount folder
//
// Archives mounted at the same place from different mount folders share one
// output folder (an OutputGroup); the highest-priority source provides a file
// that several of them hold. Output folders nest (res/ holds res/ui/), and a
// nested archive's copy of a file wins over the parent archive's.
inline constexpr std::string_view kUnnamedFolder = "_unnamed";
inline constexpr std::string_view kOverriddenFolder = "_overridden";
inline constexpr std::string_view kOutsideRecords = "_outside";
inline constexpr std::string_view kIndexFolderName = "_archives";   // the index (written by npk::pipeline)

// Where one archive belongs: in the game, in the output and in the index.
struct Location {
    std::string folder;    // output folder: "res/model/character"
    std::string label;     // path relative to the installation if inside it, else the path given
    int rank = 0;          // lower = higher priority
    std::string mount;     // mount folder relative to the installation ("Documents/res"), or ""
    std::string record;    // its index record, relative to _archives/: "res/ui.npk.csv"
};

// `order` (the archive's position in the input list) ranks archives outside the
// installation's mount folders after all of those.
[[nodiscard]] Location locate(const std::filesystem::path& archive, const Installation* installation,
                              int order = 0);

// What to do with one entry.
enum class Disposition : std::uint8_t {
    write,        // the copy that ends up in the tree
    write_copy,   // overridden, written to _overridden/ (keep_overridden)
    skip,         // overridden, not written (recorded in the index only)
};

// What happened to it; set by whoever extracts the entry. Each PlannedEntry is
// written by exactly one thread, so results need no locking.
enum class EntryStatus : std::uint8_t {
    pending,
    written,
    failed,
    existing,   // already in the output folder with the right size (from an earlier run): not rewritten
};

struct PlannedEntry {
    std::uint32_t toc_index = 0;   // into ArchiveSource::toc
    Disposition disposition = Disposition::write;
    bool unnamed = false;          // path lacks the extension; see output_path()
    std::int32_t replaced_by = -1; // overridden: index into Plan::labels of the winner
    std::string path;              // relative to the output folder ('/'); empty for skip
    EntryStatus status = EntryStatus::pending;
    std::string error;             // failed: why
};

// The final output path of an entry: path, plus ".<ext>" guessed from the
// decoded data for unnamed entries.
[[nodiscard]] std::string output_path(const PlannedEntry& entry, std::span<const std::byte> data);

// One .npk file that is part of an output group.
struct ArchiveSource {
    std::filesystem::path path;
    Location location;
    std::uint64_t file_size = 0;
    std::vector<Entry> toc;               // the whole table of contents, in table order
    std::vector<PlannedEntry> entries;    // processed entries, in storage (offset) order
    std::uint64_t packed_bytes = 0;       // of `entries`
    std::uint64_t raw_bytes = 0;

    [[nodiscard]] const Entry& entry(const PlannedEntry& planned) const { return toc[planned.toc_index]; }
    [[nodiscard]] const std::string& label() const noexcept { return location.label; }
};

// All archives mounted at the same place in the tree, unpacked into one folder.
struct OutputGroup {
    std::string folder;                 // "res/model/character"
    std::vector<ArchiveSource> sources; // highest priority first

    // The folder that holds this one: "res/model" for "res/model/character", "" at the top.
    [[nodiscard]] std::string_view section() const noexcept;
};

struct PlanTotals {
    std::uint64_t entries = 0;          // processed entries
    std::uint64_t files_to_write = 0;   // write + write_copy
    std::uint64_t overridden = 0;       // write_copy + skip
    std::uint64_t unnamed = 0;          // of the written ones
    std::uint64_t existing = 0;         // already in the output folder: not written again
    std::uint64_t existing_bytes = 0;
    std::uint64_t packed_bytes = 0;     // stored bytes to read for the written ones
    std::uint64_t raw_bytes = 0;        // bytes to write
};

struct Plan {
    std::vector<OutputGroup> groups;    // nested sections first, then by name
    std::vector<std::pair<std::filesystem::path, std::string>> unreadable;   // (archive, reason)
    std::vector<std::string> labels;    // archive labels referenced by PlannedEntry::replaced_by
    std::vector<std::string> directories;   // every folder that receives files, sorted
    PlanTotals totals;

    [[nodiscard]] std::size_t source_count() const noexcept;
};

// The files already in an output folder, so a second run into it writes only
// what is not there yet. A file counts when its size is the entry's: a file
// cut short by an interrupted run is written again.
class ExistingFiles {
public:
    // Every file below `output_root`, except the _archives/ index. Nothing if it doesn't exist.
    [[nodiscard]] static ExistingFiles scan(const std::filesystem::path& output_root);

    // Adds one file ('/' path relative to the output folder); for tests and scan().
    void add(std::string_view relative, std::uint64_t size);

    // The path of the file that `entry` (planned at `planned_path`) would be,
    // if it is there with `size` bytes: planned_path itself, or for an unnamed
    // entry the file with its guessed extension.
    [[nodiscard]] std::optional<std::string> find(std::string_view planned_path, bool unnamed, std::uint64_t size) const;

    [[nodiscard]] std::size_t size() const noexcept { return count_; }

private:
    struct File {
        std::uint64_t size = 0;
        std::string extension;   // unnamed files: ".png"; their key has none
    };
    std::unordered_map<std::string, File> files_;   // lower case on Windows
    std::size_t count_ = 0;
};

struct PlanOptions {
    bool keep_overridden = false;          // also write lower-priority copies
    std::optional<std::size_t> limit;      // only the first N entries (by offset) of each archive
    const ExistingFiles* existing = nullptr;   // leave out the entries already there (no --overwrite)
    // Called after each archive's table of contents is read (done of total); the
    // plan is decided after the last one.
    std::function<void(std::size_t done, std::size_t total)> progress;
};

// Turns a list of archives into a Plan: reads every table of contents, places
// each archive in the tree, recovers names, and decides for every entry whether
// and where it is written.
//
// Overrides are decided up front from the tables of contents: within a group
// the first source (by priority) holding an id provides it. Unlike npkext, a
// provider whose entry later fails to decode does not hand the file to the next
// copy; the failure is reported instead.
class Planner {
public:
    // `installation` may be null (loose archives). The manifest must outlive the planner.
    Planner(const ResourceManifest& manifest, const Installation* installation);

    [[nodiscard]] Plan build(std::span<const std::filesystem::path> archives, const PlanOptions& options);

private:
    NameResolver names_;
    const Installation* installation_;
};

} // namespace npk
