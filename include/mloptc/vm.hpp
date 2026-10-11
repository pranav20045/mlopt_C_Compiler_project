#pragma once

#include "mloptc/execution.hpp"
#include "mloptc/ir.hpp"

namespace mloptc {

struct VmOptions {
  /// Abort after this many IR instructions. See InterpreterOptions::maxSteps.
  std::uint64_t maxInstructions = 500'000'000ULL;
};

/// Executes an IR module.
///
/// This is the SECOND execution path, and it is deliberately independent of the
/// AST interpreter: it shares the arithmetic helpers in execution.hpp (so
/// wrapping and trapping cannot drift) but nothing else. Two implementations
/// that shared their logic would agree with each other even when both were
/// wrong, which would make the differential test worthless.
///
/// It also counts instructions executed, and that number is the cost signal the
/// whole ML half of this project optimizes. It is a proxy for runtime, not a
/// measurement of it -- but it has the enormous advantage of being exactly
/// reproducible, so a model trained on it is not learning the machine's
/// scheduling noise.
[[nodiscard]] ExecutionResult execute(const ir::Module& module, const VmOptions& options = {});

} // namespace mloptc
