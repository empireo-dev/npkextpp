#pragma once

#include <string_view>

namespace npkextpp {

// License texts of the third-party libraries linked into the executable.
[[nodiscard]] std::string_view third_party_licenses() noexcept;

} // namespace npkextpp
