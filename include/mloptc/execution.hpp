#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace mloptc {

/// Why a run stopped. Every backend must agree on these (docs/spec.md 7.6).
enum class RuntimeStatus : std::uint8_t {
  Ok,
  DivisionByZero,
  RemainderByZero,
  DivisionOverflow, ///< INT_MIN / -1
  IndexOutOfBounds,
  CallDepthExceeded,
  StepLimitExceeded, ///< not a ToyC error: the harness ran out of patience
};

[[nodiscard]] std::string_view runtimeStatusMessage(RuntimeStatus status) noexcept;

/// The exit status the spec assigns to any runtime error.
inline constexpr int kRuntimeErrorExitStatus = 70;

/// Exit status used when a backend gives up because the program ran too long.
///
/// This must NOT be 70. A step-limit abort is not a ToyC runtime error -- the
/// program is simply still running, which docs/spec.md 7.7 explicitly allows --
/// and the point where each backend gives up differs, since the AST
/// interpreter counts node visits, the VM counts IR instructions, and native
/// code has no counter at all. Sharing status 70 with real traps made every
/// non-terminating program look like a disagreement about trapping, which the
/// fuzzer duly reported. 124 is the conventional "timed out" status, and the
/// differential tester treats it as "no result" rather than as a mismatch.
inline constexpr int kStepLimitExitStatus = 124;

/// Per docs/spec.md section 7.5.
///
/// 1000, not something larger, and the reason is worth stating: the AST
/// interpreter implements a ToyC call as native C++ recursion, so each ToyC
/// frame costs several C++ frames. With a limit of 10000 the interpreter
/// overflowed the host stack -- a hard crash -- *before* this counter could
/// trip, which made the guard useless exactly when it was needed. The limit
/// must be low enough that every backend reaches it while still healthy.
/// Executables also link with an enlarged stack; see cmake/StackSize.cmake.
inline constexpr std::uint32_t kMaxCallDepth = 1000;

/// The observable behaviour of one execution.
///
/// This struct *is* the contract the differential tester checks. Per
/// docs/spec.md section 8, two executions are equivalent iff they agree on
/// `output` and `exitStatus`. Everything else here is diagnostic or
/// measurement, and is deliberately excluded from that comparison -- in
/// particular `dynamicInstructions`, which is the cost signal the ML pass
/// scheduler optimizes and which is *expected* to differ between backends.
struct ExecutionResult {
  std::string output; ///< everything written by print()
  int exitStatus = 0; ///< main's return value mod 256, or 70 on error
  RuntimeStatus status = RuntimeStatus::Ok;

  /// Dynamic instruction count. Meaningful for the IR VM; the AST interpreter
  /// reports a rough node-visit count, which is useful for spotting runaway
  /// programs but is not comparable across backends.
  std::uint64_t dynamicInstructions = 0;

  [[nodiscard]] bool ok() const noexcept { return status == RuntimeStatus::Ok; }

  /// True if two runs are equivalent per the spec's definition.
  [[nodiscard]] bool observablyEquals(const ExecutionResult& other) const noexcept {
    return output == other.output && exitStatus == other.exitStatus;
  }
};

/// Shared by every backend, so wrapping behaviour cannot drift between them.
///
/// The casts are not decoration. Signed overflow is undefined behaviour in C++,
/// so `a + b` on two large ints would be UB in our *own* compiler -- and a
/// UBSan build would abort on it. Unsigned arithmetic is defined to wrap
/// modulo 2^32, and in C++20 the conversion back to int32_t is defined to be
/// the modular result rather than implementation-defined. That gives us exactly
/// the two's-complement wrapping docs/spec.md section 7.1 requires.
[[nodiscard]] constexpr std::int32_t wrapAdd(std::int32_t a, std::int32_t b) noexcept {
  return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}

[[nodiscard]] constexpr std::int32_t wrapSub(std::int32_t a, std::int32_t b) noexcept {
  return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) - static_cast<std::uint32_t>(b));
}

[[nodiscard]] constexpr std::int32_t wrapMul(std::int32_t a, std::int32_t b) noexcept {
  return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) * static_cast<std::uint32_t>(b));
}

[[nodiscard]] constexpr std::int32_t wrapNeg(std::int32_t a) noexcept {
  return static_cast<std::int32_t>(0U - static_cast<std::uint32_t>(a));
}

/// Division, with the two cases the spec defines as traps. Returns false and
/// sets `status` rather than trapping, so each backend decides how to react.
[[nodiscard]] constexpr bool checkedDiv(std::int32_t a, std::int32_t b, std::int32_t& out,
                                        RuntimeStatus& status) noexcept {
  if (b == 0) {
    status = RuntimeStatus::DivisionByZero;
    return false;
  }
  if (a == INT32_MIN && b == -1) {
    // The true result, 2147483648, does not fit in int32. On x86 the hardware
    // divide instruction faults here, so defining it as a trap costs nothing
    // and keeps every backend honest.
    status = RuntimeStatus::DivisionOverflow;
    return false;
  }
  out = a / b;
  return true;
}

[[nodiscard]] constexpr bool checkedRem(std::int32_t a, std::int32_t b, std::int32_t& out,
                                        RuntimeStatus& status) noexcept {
  if (b == 0) {
    status = RuntimeStatus::RemainderByZero;
    return false;
  }
  if (a == INT32_MIN && b == -1) {
    // Mathematically 0, but the same hardware instruction computes it, so it
    // faults identically. Defined as a trap for the same reason.
    status = RuntimeStatus::DivisionOverflow;
    return false;
  }
  out = a % b;
  return true;
}

} // namespace mloptc
