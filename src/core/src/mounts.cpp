#include "npk/mounts.hpp"

#include "npk/paths.hpp"

#include <algorithm>
#include <array>

namespace npk {

namespace {

// Copied from the log of one game session (PC client 1.5.21.3), in mount order.
constexpr std::array<Mount, 25> kMounts{{
    {"Documents/res_dlc_low", 0, Tree::resource},
    {"Documents/gray_res", 0, Tree::resource},
    {"Documents/res4", 2, Tree::resource},
    {"Documents/res3", 2, Tree::resource},
    {"Documents/res2", 2, Tree::resource},
    {"Documents/res", 2, Tree::resource},
    {"Documents/res_silent1", 4, Tree::resource},
    {"Documents/res_dlc_dress", 3, Tree::resource},
    {"Documents/res_dlc_scene", 3, Tree::resource},
    {"Documents/res_dlc_y2", 3, Tree::resource},
    {"Documents/res_dlc_patch", 3, Tree::resource},
    {"Documents/res_dlc", 3, Tree::resource},
    {"Documents/res_silent3_patch", 2, Tree::resource},
    {"Documents/res_silent3", 2, Tree::resource},
    {"Documents/res_silent_patch", 2, Tree::resource},
    {"Documents/res_silent", 2, Tree::resource},
    {"res", 2, Tree::resource},
    {"res2", 2, Tree::resource},
    {"res3", 2, Tree::resource},
    {"res4", 2, Tree::resource},
    {"Documents/gray_script_patch", 0, Tree::script},
    {"Documents/gray_script", 0, Tree::script},
    {"Documents/script_patch", 0, Tree::script},
    {"Documents/script", 0, Tree::script},
    {"script", 0, Tree::script},
}};

// A folder name used twice ("res" in Documents/ and in the root) has the same
// depth and tree, so the first mount with a name stands for all of them.
const Mount* mount_by_name(std::string_view name)
{
    for (const Mount& mount : kMounts) {
        if (iequals_ascii(mount.name(), name)) return &mount;
    }
    return nullptr;
}

std::filesystem::path join(std::span<const std::string> parts)
{
    std::filesystem::path out;
    for (const std::string& part : parts) out /= from_utf8(part);
    return out;
}

} // namespace

std::string_view tree_folder(Tree tree) noexcept
{
    return tree == Tree::resource ? "res" : "script";
}

std::string_view Mount::name() const noexcept
{
    const auto slash = root.rfind('/');
    return slash == std::string_view::npos ? root : root.substr(slash + 1);
}

std::span<const Mount> mounts() noexcept { return kMounts; }

int max_mount_depth() noexcept
{
    static const int depth = std::ranges::max(kMounts, {}, &Mount::depth).depth;
    return depth;
}

std::optional<int> mount_rank(std::string_view mount_root)
{
    std::string root(mount_root);
    std::ranges::replace(root, '\\', '/');
    while (root.size() > 1 && root.back() == '/') root.pop_back();
    for (std::size_t i = 0; i < kMounts.size(); ++i) {
        if (iequals_ascii(kMounts[i].root, root)) return static_cast<int>(i);
    }
    return std::nullopt;
}

std::string Placement::output_folder() const
{
    std::string out(tree_folder(mount->tree));
    for (const std::string& part : branch) {
        out += '/';
        out += part;
    }
    return out;
}

std::optional<std::string> Placement::name_prefix() const
{
    if (mount->tree != Tree::resource) return std::nullopt;
    std::string prefix;
    for (const std::string& part : branch) {
        prefix += part;
        prefix += '\\';
    }
    return prefix;
}

std::optional<Placement> place(const std::filesystem::path& archive)
{
    const std::vector<std::string> parts = path_parts(archive);
    if (parts.empty()) return std::nullopt;
    const std::string stem = to_utf8(from_utf8(parts.back()).stem());
    const int count = static_cast<int>(parts.size());

    // Inside a mount folder: R/ui.npk is one level below R, R/model/character.npk two.
    const int levels = std::min(max_mount_depth(), count - 1);
    for (int level = 1; level <= levels; ++level) {
        const Mount* mount = mount_by_name(parts[static_cast<std::size_t>(count - 1 - level)]);
        if (mount != nullptr && level <= mount->depth) {
            Placement placement;
            placement.mount = mount;
            placement.root = join(std::span(parts).first(static_cast<std::size_t>(count - level)));
            placement.branch.assign(parts.end() - level, parts.end() - 1);
            placement.branch.push_back(stem);
            return placement;
        }
    }

    // Next to a mount folder: R.npk holds the root of the tree.
    if (const Mount* mount = mount_by_name(stem); mount != nullptr) {
        Placement placement;
        placement.mount = mount;
        placement.root = join(std::span(parts).first(parts.size() - 1)) / from_utf8(stem);
        return placement;
    }
    return std::nullopt;
}

} // namespace npk
