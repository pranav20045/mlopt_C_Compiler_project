#pragma once

#include "mloptc/ast.hpp"
#include "mloptc/diagnostics.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace mloptc {

/// Runs the whole front end: lex, parse, analyze.
///
/// Returns nullopt if the program has any error. This exists so that tests, the
/// CLI and the differential tester all drive the front end through exactly one
/// path -- if they each wired up the stages themselves, they could drift apart
/// and a test would stop testing what the CLI actually does.
[[nodiscard]] std::optional<Program> compileFrontend(std::string_view source,
                                                     DiagnosticEngine& diags);

/// Reads a file into a string. Returns nullopt if it cannot be opened.
[[nodiscard]] std::optional<std::string> readFile(const std::string& path);

} // namespace mloptc
