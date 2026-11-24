#pragma once

#include "npk/error.hpp"

#include <filesystem>
#include <optional>
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
    // Opens `folder` (made absolute, symlinks resolved) and checks that it looks
    // like an installation.
    [[nodiscard]] static Result<Installation> open(const std::filesystem::path& folder);

    // The installation that contains `path`, found by walking up its folders.
    [[nodiscard]] static std::optional<Installation> find_above(const std::filesystem::path& path);

    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

    // neox_bin/ exists, or res/resdb + Documents/ do.
    [[nodiscard]] bool looks_valid() const;

    // Every .npk file below the root, sorted.
    [[nodiscard]] std::vector<std::filesystem::path> archives() const;

    // The resource manifests that exist, patched one first.
    [[nodiscard]] std::vector<std::filesystem::path> resdb_files() const;

    // `path` relative to the root (symlinks resolved), or nullopt if it lies outside.
    [[nodiscard]] std::optional<std::filesystem::path> relative(const std::filesystem::path& path) const;

    // Priority of the mount folder an archive belongs to (0 = highest), or nullopt
    // if it lies outside the installation or outside every known mount folder.
    [[nodiscard]] std::optional<int> mount_rank(const std::filesystem::path& archive) const;

private:
    explicit Installation(std::filesystem::path root) : root_(std::move(root)) {}
    std::filesystem::path root_;
};

} // namespace npk
