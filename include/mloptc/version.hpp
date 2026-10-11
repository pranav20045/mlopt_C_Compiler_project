#pragma once

#include <string_view>

namespace mloptc {

/// Semantic version of the MLOpt-C toolchain, e.g. "0.1.0".
[[nodiscard]] std::string_view version() noexcept;

/// Human-readable banner printed by `mloptc --version`.
[[nodiscard]] std::string_view versionBanner() noexcept;

} // namespace mloptc
