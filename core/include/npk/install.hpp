#pragma once

#include "npk/error.hpp"

#include <filesystem>
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

// A game installation folder: the one containing neox_bin/, res/ and Documents/.
class Installation {
public:
    // Opens `folder` (made absolute, symlinks resolved), checks that it looks like
    // an installation and finds its archives by walking it for .npk files.
    [[nodiscard]] static Result<Installation> open(const std::filesystem::path& folder);

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
    // Walks the root for .npk files; notes the folders it cannot open.
    void scan_archives();

    std::filesystem::path root_;
    std::vector<std::filesystem::path> archives_;
    std::vector<std::string> unreadable_;
};

} // namespace npk
