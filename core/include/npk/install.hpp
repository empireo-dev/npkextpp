#pragma once

#include "npk/error.hpp"

#include <filesystem>
#include <vector>

namespace npk {

// A game installation on disk: where the archives and the resdb manifests are.
class Installation {
public:
    [[nodiscard]] static Result<Installation> locate(const std::filesystem::path& root);

    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

    // The resdb manifests, patched one first.
    [[nodiscard]] std::vector<std::filesystem::path> resdb_files() const;

    // Every .npk file below the installation root.
    [[nodiscard]] Result<std::vector<std::filesystem::path>> find_archives() const;

private:
    std::filesystem::path root_;
};

} // namespace npk
