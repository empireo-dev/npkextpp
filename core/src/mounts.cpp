#include "npk/mounts.hpp"

#include <array>

namespace npk {

namespace {
// TODO: full table from the engine's [FILESYSTEM] log (see npkext mounts.py).
constexpr std::array<Mount, 1> kMounts{{
    {"res", 2, Tree::resource},
}};
} // namespace

std::span<const Mount> mounts() noexcept { return kMounts; }

} // namespace npk
