#pragma once

#include "npk/error.hpp"
#include "npk/plan.hpp"

#include <filesystem>

namespace npk::pipeline {

// Writes the _archives/ index (index.csv, mounts.csv, one record per archive)
// from a finished Plan, so the archives can be rebuilt later.
[[nodiscard]] Result<void> write_index(const Plan& plan, const std::filesystem::path& output_root);

} // namespace npk::pipeline
