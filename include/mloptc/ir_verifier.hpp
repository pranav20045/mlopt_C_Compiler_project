#pragma once

#include "mloptc/ir.hpp"

#include <string>
#include <vector>

namespace mloptc::ir {

/// Structural checks on a function's IR.
///
/// This is the project's second line of defence, and it catches a different
/// class of bug from the differential tester. The differential tester finds
/// passes that change what a program *computes*; the verifier finds passes that
/// leave the IR *malformed* -- a block with two terminators, a jump to a block
/// that no longer exists, a register used before it is defined. Those bugs
/// often do not change the answer on the one test that happens to run, and then
/// blow up months later on different input.
///
/// Running it after every pass in debug builds means a broken pass is caught on
/// the spot, with the name of the pass that broke it, instead of being blamed
/// on whichever pass runs next.
///
/// Checks performed:
///   - every block ends with exactly one terminator, and has no terminator
///     in the middle
///   - every branch and jump target exists
///   - every ValueId is defined exactly once  (the "pseudo-SSA" property the
///     optimization passes rely on)
///   - every register is defined before it is used, in reverse-postorder --
///     which for our reducible CFGs means along every path that can reach it
///   - slot indices are within the frame, and global indices within the module
///   - the entry block has no predecessors other than itself via a back edge
class VerificationResult {
public:
  [[nodiscard]] bool ok() const noexcept { return errors_.empty(); }
  [[nodiscard]] const std::vector<std::string>& errors() const noexcept { return errors_; }

  /// All errors joined with newlines, for an assertion message.
  [[nodiscard]] std::string message() const;

  void addError(std::string error) { errors_.push_back(std::move(error)); }

private:
  std::vector<std::string> errors_;
};

[[nodiscard]] VerificationResult verify(const Function& fn, std::uint32_t globalCount);
[[nodiscard]] VerificationResult verify(const Module& module);

} // namespace mloptc::ir
