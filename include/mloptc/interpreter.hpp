#pragma once

#include "mloptc/ast.hpp"
#include "mloptc/execution.hpp"

#include <cstdint>

namespace mloptc {

struct InterpreterOptions {
  /// Abort after this many AST node visits. Fuzzed programs loop forever
  /// often enough that an unbounded interpreter would wedge the test harness;
  /// the spec explicitly allows a harness timeout (section 7.7).
  std::uint64_t maxSteps = 500'000'000ULL;
};

/// Executes a program directly on its AST.
///
/// This is the REFERENCE ORACLE. It is written to be obviously correct rather
/// than fast: it walks the tree, it does no optimization, and every rule in
/// docs/spec.md maps to a visible piece of code here. When the IR VM or the
/// LLVM backend disagrees with this interpreter, the assumption is that *they*
/// are wrong -- which is only a safe assumption because this code never tries
/// to be clever.
///
/// The program must already have passed `analyze()`; the interpreter assumes
/// names are resolved and the AST is well-typed.
[[nodiscard]] ExecutionResult interpret(const Program& program,
                                        const InterpreterOptions& options = {});

} // namespace mloptc
