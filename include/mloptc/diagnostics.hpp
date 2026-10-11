#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>
#include <string_view>
#include <vector>

namespace mloptc {

/// A position in a source file. Lines and columns are 1-based, because that is
/// what every editor and every compiler diagnostic uses.
///
/// Note the name: not `SourceLocation`, to avoid confusion with C++20's
/// `std::source_location`, which is about *this* compiler's own source, not the
/// ToyC program being compiled.
struct SourceLoc {
  std::uint32_t line = 1;
  std::uint32_t column = 1;

  friend bool operator==(const SourceLoc&, const SourceLoc&) = default;
};

enum class Severity : std::uint8_t { Error, Warning, Note };

struct Diagnostic {
  Severity severity = Severity::Error;
  SourceLoc loc;
  std::string message;
};

/// Collects diagnostics rather than throwing or calling exit().
///
/// Why collect instead of bailing out at the first error? Because a compiler
/// that reports one error per run is painful to use, and -- more importantly
/// here -- because the test suite needs to assert on *which* errors a bad
/// program produces. Exceptions would make that awkward; a vector does not.
class DiagnosticEngine {
public:
  void error(SourceLoc loc, std::string message);
  void warning(SourceLoc loc, std::string message);
  void note(SourceLoc loc, std::string message);

  [[nodiscard]] bool hasErrors() const noexcept { return errorCount_ > 0; }

  [[nodiscard]] std::size_t errorCount() const noexcept { return errorCount_; }

  [[nodiscard]] const std::vector<Diagnostic>& all() const noexcept { return diagnostics_; }

  /// Writes every diagnostic in `file:line:col: severity: message` form, which
  /// is the format editors and CI log scrapers already understand.
  void print(std::ostream& os, std::string_view filename) const;

  void clear() noexcept;

private:
  std::vector<Diagnostic> diagnostics_;
  std::size_t errorCount_ = 0;
};

} // namespace mloptc
