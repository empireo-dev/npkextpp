#pragma once

#include "npk/error.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace npk {

// Where the resdb manifests are, relative to the installation; patched one first.
inline constexpr const char* kResdbLocations[] = {
    "Documents/res/resdb",   // patched manifest (newer, larger)
    "res/resdb",             // manifest shipped with the installer
};

// A folder holding a file of this name is left out of every walk, with all its
// subfolders. npkextpp puts one into every folder it unpacks into.
inline constexpr const char* kNoScanFileName = ".noscan";
inline constexpr const char* kNoScanText =
    "This file makes the archive scanner of npkextpp skip this folder and all of its\n"
    "subfolders, recursively: no .npk archives or resdb name manifests are looked for\n"
    "anywhere below it.\n"
    "\n"
    "npkextpp puts it into every folder it unpacks archives into, so the unpacked files\n"
    "are not searched through again on the next run. Delete it to have this folder\n"
    "scanned again.\n";

// Writes `folder`/.noscan (creating the folder) unless it is already there.
[[nodiscard]] Result<void> mark_no_scan(const std::filesystem::path& folder);

// What a walk of a folder found (symlinks are not followed).
struct FolderScan {
    std::vector<std::filesystem::path> archives;    // .npk files, sorted
    std::vector<std::filesystem::path> manifests;   // name manifests (files named resdb that are one), sorted
    std::vector<std::string> unreadable;            // folders that could not be opened, '/' relative, sorted
    std::vector<std::string> skipped;               // folders left out for their .noscan file, '/' relative, sorted
};

// Called during a walk with what was found so far (not sorted yet), after every folder.
using ScanProgress = std::function<void(const FolderScan& so_far)>;

// Walks `root` for archives and name manifests, leaving out every folder that
// holds a .noscan file (the root too: then nothing is found).
[[nodiscard]] FolderScan scan_folder(const std::filesystem::path& root, const ScanProgress& progress = {});

// A game installation folder: the one containing neox_bin/, res/ and Documents/.
class Installation {
public:
    // Opens `folder` (made absolute, symlinks resolved), checks that it looks like
    // an installation and walks it for archives and name manifests (see scan_folder).
    [[nodiscard]] static Result<Installation> open(const std::filesystem::path& folder,
                                                   const ScanProgress& progress = {});

    // `folder` has neox_bin/, or res/resdb + Documents/ (checked without walking it).
    [[nodiscard]] static bool looks_like_one(const std::filesystem::path& folder);

    // The installation that contains `path`, found by walking up its folders.
    [[nodiscard]] static std::optional<Installation> find_above(const std::filesystem::path& path);

    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

    // neox_bin/ exists, or res/resdb + Documents/ do.
    [[nodiscard]] bool looks_valid() const;

    // The .npk files found below the root when it was opened, sorted.
    [[nodiscard]] const std::vector<std::filesystem::path>& archives() const noexcept { return archives_; }

    // Folders of the installation that exist but could not be opened (permission
    // denied), '/' relative, sorted: archives in them are not found.
    [[nodiscard]] const std::vector<std::string>& unreadable_folders() const noexcept { return unreadable_; }
    // Folders left out for their .noscan file, '/' relative, sorted.
    [[nodiscard]] const std::vector<std::string>& skipped_folders() const noexcept { return skipped_; }

    // The name manifests found when it was opened (Documents/res/resdb and
    // res/resdb in the game), sorted.
    [[nodiscard]] const std::vector<std::filesystem::path>& resdb_files() const noexcept { return manifests_; }
    // Manifest locations (kResdbLocations) that cannot be checked: permission denied.
    [[nodiscard]] std::vector<std::string> unreadable_resdb() const;

    // `path` relative to the root (symlinks resolved), or nullopt if it lies outside.
    [[nodiscard]] std::optional<std::filesystem::path> relative(const std::filesystem::path& path) const;

    // Priority of the mount folder an archive belongs to (0 = highest), or nullopt
    // if it lies outside the installation or outside every known mount folder.
    [[nodiscard]] std::optional<int> mount_rank(const std::filesystem::path& archive) const;

private:
    explicit Installation(std::filesystem::path root) : root_(std::move(root)) {}
    void scan(const ScanProgress& progress);

    std::filesystem::path root_;
    std::vector<std::filesystem::path> archives_;
    std::vector<std::filesystem::path> manifests_;
    std::vector<std::string> unreadable_;
    std::vector<std::string> skipped_;
};

} // namespace npk
