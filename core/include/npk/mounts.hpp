#pragma once

#include <span>
#include <string_view>

namespace npk {

// Which virtual file tree a mount folder fills.
enum class Tree {
    resource,
    script,
};

// A folder the engine creates a file loader for. A mount folder R with depth d
// loads R.npk (next to R, at the tree root) and archives up to d folder levels
// below R, each at its relative folder path.
struct Mount {
    std::string_view root;  // relative to the installation, forward slashes
    int depth;
    Tree tree;
};

// All mount folders in the order the engine creates them (= priority order).
[[nodiscard]] std::span<const Mount> mounts() noexcept;

} // namespace npk
