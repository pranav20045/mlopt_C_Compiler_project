#include "mloptc/diagnostics.hpp"

#include <ostream>
#include <utility>

namespace mloptc {

namespace {

std::string_view severityName(Severity s) {
  switch (s) {
  case Severity::Error:
    return "error";
  case Severity::Warning:
    return "warning";
  case Severity::Note:
    return "note";
  }
  return "unknown";
}

} // namespace

void DiagnosticEngine::error(SourceLoc loc, std::string message) {
  diagnostics_.push_back({Severity::Error, loc, std::move(message)});
  ++errorCount_;
}

void DiagnosticEngine::warning(SourceLoc loc, std::string message) {
  diagnostics_.push_back({Severity::Warning, loc, std::move(message)});
}

void DiagnosticEngine::note(SourceLoc loc, std::string message) {
  diagnostics_.push_back({Severity::Note, loc, std::move(message)});
}

void DiagnosticEngine::print(std::ostream& os, std::string_view filename) const {
  for (const Diagnostic& d : diagnostics_) {
    os << filename << ':' << d.loc.line << ':' << d.loc.column << ": " << severityName(d.severity)
       << ": " << d.message << '\n';
  }
}

void DiagnosticEngine::clear() noexcept {
  diagnostics_.clear();
  errorCount_ = 0;
}

} // namespace mloptc
