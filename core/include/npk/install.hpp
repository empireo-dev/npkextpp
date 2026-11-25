#pragma once

#include "npk/error.hpp"

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace npk {

// Where the resdb manifests are, relative to the installation; patched one first.
inline constexpr const char* kResdbLocations[] = {
    "Documents/res/resdb",   // patched manifest (newer, larger)
    "res/resdb",             // manifest shipped with the installer
};

// How an installation's archives are found.
enum class ArchiveSearch {
    known,   // check the built-in list of the game's archives (fast); scan if none of them is there
    scan,    // walk the whole installation for .npk files
};

// The game's archives, relative to the installation ('/' separated), from
// core/data/known_archives.txt (compiled in).
[[nodiscard]] std::span<const std::string> known_archives();

// A game installation folder: the one containing neox_bin/, res/ and Documents/.
class Installation {
public:
    // Opens `folder` (made absolute, symlinks resolved), checks that it looks like
    // an installation and finds its archives. `known` replaces the built-in list (tests).
    [[nodiscard]] static Result<Installation> open(const std::filesystem::path& folder,
                                                   ArchiveSearch search = ArchiveSearch::known);
    [[nodiscard]] static Result<Installation> open(const std::filesystem::path& folder, ArchiveSearch search,
                                                   std::span<const std::string> known);

    // The installation that contains `path`, found by walking up its folders.
    [[nodiscard]] static std::optional<Installation> find_above(const std::filesystem::path& path);

    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

    // neox_bin/ exists, or res/resdb + Documents/ do.
    [[nodiscard]] bool looks_valid() const;

    // The archives found when it was opened, sorted.
    [[nodiscard]] const std::vector<std::filesystem::path>& archives() const noexcept { return archives_; }
    // Archives of the known list that are not there ('/' relative paths), sorted.
    [[nodiscard]] const std::vector<std::string>& missing_archives() const noexcept { return missing_; }
    // The installation was walked (asked for, or none of the known archives was there).
    [[nodiscard]] bool scanned() const noexcept { return scanned_; }
    [[nodiscard]] std::size_t known_count() const noexcept { return known_count_; }

    // Folders of the installation that exist but could not be opened (permission
    // denied), '/' relative, sorted: archives in them are not found.
    [[nodiscard]] const std::vector<std::string>& unreadable_folders() const noexcept { return unreadable_; }

    // Every .npk file below the root, sorted (walks the folder; notes the folders it cannot open).
    [[nodiscard]] std::vector<std::filesystem::path> scan_archives();

    // The resource manifests that exist, patched one first.
    [[nodiscard]] std::vector<std::filesystem::path> resdb_files() const;
    // Manifest locations (kResdbLocations) that cannot be checked: permission denied.
    [[nodiscard]] std::vector<std::string> unreadable_resdb() const;

    // `path` relative to the root (symlinks resolved), or nullopt if it lies outside.
    [[nodiscard]] std::optional<std::filesystem::path> relative(const std::filesystem::path& path) const;

    // Priority of the mount folder an archive belongs to (0 = highest), or nullopt
    // if it lies outside the installation or outside every known mount folder.
    [[nodiscard]] std::optional<int> mount_rank(const std::filesystem::path& archive) const;

private:
    explicit Installation(std::filesystem::path root) : root_(std::move(root)) {}
    void find_archives(ArchiveSearch search, std::span<const std::string> known);
    void note_if_blocked(const std::string& relative);

    std::filesystem::path root_;
    std::vector<std::filesystem::path> archives_;
    std::vector<std::string> missing_;
    std::vector<std::string> unreadable_;
    std::size_t known_count_ = 0;
    bool scanned_ = false;
};

} // namespace npk
