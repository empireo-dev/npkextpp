#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace npk {

// Which virtual file tree a mount folder fills, and the output folder for it.
enum class Tree {
    resource,   // "res": named by resdb
    script,     // "script": resdb does not list scripts
};

[[nodiscard]] std::string_view tree_folder(Tree tree) noexcept;

// A folder the engine creates a file loader for (from the [FILESYSTEM] lines of
// the game's log.txt). A mount folder R with depth d loads:
//   R.npk (next to R)           -> the root of the tree
//   R/<a>.npk                   -> branch <a>
//   R/<a>/<b>.npk               -> branch <a>/<b>
//   ... up to d folder levels below R
struct Mount {
    std::string_view root;   // relative to the installation, '/' separated
    int depth;
    Tree tree;

    // The folder name: "res" for "Documents/res".
    [[nodiscard]] std::string_view name() const noexcept;
};

// Every mount folder in the order the engine mounts them: highest priority first.
// Assumption (as in npkext): the first-mounted folder wins when several hold
// the same file.
[[nodiscard]] std::span<const Mount> mounts() noexcept;

// The deepest depth of any mount folder.
[[nodiscard]] int max_mount_depth() noexcept;

// Priority of a mount folder given relative to the installation ("Documents/res"):
// 0 is the highest. nullopt if it is not a known mount folder. Ignores case.
[[nodiscard]] std::optional<int> mount_rank(std::string_view mount_root);

// Where one archive is mounted.
struct Placement {
    const Mount* mount = nullptr;
    std::filesystem::path root;          // the mount folder, as found in the archive's path
    std::vector<std::string> branch;     // {} for R.npk, {"model", "character"}

    // "res/model/character" for Documents/res/model/character.npk.
    [[nodiscard]] std::string output_folder() const;

    // How the archive's files are listed in resdb: "model\\character\\", "" for
    // the root, nullopt for trees that resdb does not cover (scripts).
    [[nodiscard]] std::optional<std::string> name_prefix() const;
};

// Where `archive` is mounted, found from the folder names in its path (so it
// also works for a copy kept in the same folder structure outside the
// installation). nullopt if it is not inside or next to a known mount folder.
[[nodiscard]] std::optional<Placement> place(const std::filesystem::path& archive);

} // namespace npk
