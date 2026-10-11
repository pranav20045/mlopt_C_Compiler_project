#pragma once

#include "mloptc/ir.hpp"

#include <string>

namespace mloptc {

/// Emits textual LLVM IR for a module.
///
/// We emit TEXT and hand it to `clang`, rather than linking against the LLVM
/// C++ API. That keeps the project dependency-free and buildable anywhere
/// clang is installed, and it makes the output something you can read and
/// check by hand -- which matters when a backend bug has to be diagnosed.
///
/// THE TRAP THIS BACKEND MUST AVOID, flagged since docs/spec.md 7.1:
///
/// ToyC arithmetic wraps. LLVM's `add nsw` means "assume signed overflow never
/// happens", which licenses the optimizer to do things like fold
/// `x + 1 > x` to `true`. If we emitted `nsw`, clang -O2 would produce code
/// that disagrees with the oracle on exactly the programs that overflow --
/// a miscompilation introduced by the backend, not by our optimizer, and one
/// that only shows up under optimization. So every arithmetic instruction here
/// is emitted WITHOUT nsw/nuw.
///
/// Likewise, ToyC traps on division by zero and on out-of-bounds indices, so
/// the emitted code performs those checks explicitly and calls a runtime
/// helper that prints the diagnostic and exits 70. LLVM's `sdiv` is undefined
/// on a zero divisor, so leaving the check out would be undefined behaviour,
/// not a trap.
[[nodiscard]] std::string emitLlvmIr(const ir::Module& module);

} // namespace mloptc
