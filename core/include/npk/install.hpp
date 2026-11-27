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

// What a walk of a folder found (symlinks are not followed).
struct FolderScan {
    std::vector<std::filesystem::path> archives;    // .npk files, sorted
    std::vector<std::filesystem::path> manifests;   // name manifests (files named resdb that are one), sorted
    std::vector<std::string> unreadable;            // folders that could not be opened, '/' relative, sorted
};

// Called during a walk with the counts so far, after every folder.
using ScanProgress = std::function<void(std::size_t archives, std::size_t manifests)>;

// Walks `root` for archives and name manifests. Left out: the folders in
// `skip`, and every unpacked output (a folder with an _archives/ folder).
[[nodiscard]] FolderScan scan_folder(const std::filesystem::path& root, const ScanProgress& progress = {},
                                     const std::vector<std::filesystem::path>& skip = {});

// A game installation folder: the one containing neox_bin/, res/ and Documents/.
class Installation {
public:
    // Opens `folder` (made absolute, symlinks resolved), checks that it looks like
    // an installation and walks it for archives and name manifests (see scan_folder).
    [[nodiscard]] static Result<Installation> open(const std::filesystem::path& folder,
                                                   const ScanProgress& progress = {},
                                                   const std::vector<std::filesystem::path>& skip = {});

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
    void scan(const ScanProgress& progress, const std::vector<std::filesystem::path>& skip);

    std::filesystem::path root_;
    std::vector<std::filesystem::path> archives_;
    std::vector<std::filesystem::path> manifests_;
    std::vector<std::string> unreadable_;
};

} // namespace npk
