#include "corpus.hpp"

#include "mloptc/driver.hpp"
#include "mloptc/llvm_emitter.hpp"
#include "mloptc/lowering.hpp"
#include "mloptc/pass_manager.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace mloptc;

namespace {

std::string emitFor(std::string_view source, const std::vector<std::string>& passes = {}) {
  DiagnosticEngine diags;
  auto program = compileFrontend(source, diags);
  if (!program) {
    ADD_FAILURE() << "front end rejected the program";
    return {};
  }

  ir::Module module = lower(*program);
  if (!passes.empty()) {
    opt::PipelineStats stats;
    std::string error;
    if (!opt::optimizeModule(module, passes, stats, error)) {
      ADD_FAILURE() << error;
      return {};
    }
  }
  return emitLlvmIr(module);
}

} // namespace

// =============================================================================
// The nsw regression test -- the most important assertion in this file
// =============================================================================

TEST(LlvmEmitter, NeverEmitsNoSignedWrapFlags) {
  // ToyC arithmetic wraps (spec 7.1). `nsw` would tell LLVM to assume signed
  // overflow cannot happen, letting clang -O2 fold things like `x + 1 > x` to
  // true -- producing a binary that disagrees with the oracle on exactly the
  // programs that overflow. The bug would be invisible at -O0 and appear only
  // under optimization, which is the worst possible failure mode.
  //
  // Checked over the entire corpus, at -O0 and -O2, so no future change to the
  // emitter can reintroduce it quietly.
  for (const auto& [name, source] : test::corpus()) {
    SCOPED_TRACE(std::string("program: ") + std::string(name));

    for (const std::vector<std::string>& passes :
         {std::vector<std::string>{}, opt::defaultO2Pipeline().passNames}) {
      const std::string llvm = emitFor(source, passes);
      EXPECT_EQ(llvm.find(" nsw "), std::string::npos) << "emitted 'nsw' for " << name;
      EXPECT_EQ(llvm.find(" nuw "), std::string::npos) << "emitted 'nuw' for " << name;
    }
  }
}

TEST(LlvmEmitter, DivisionGoesThroughTheCheckedHelper) {
  // A bare `sdiv` is undefined on a zero divisor, so emitting one would turn a
  // specified trap into undefined behaviour that clang is free to assume never
  // happens.
  const std::string llvm = emitFor("int main() { int d = 0; print(10 / d); return 0; }");

  EXPECT_NE(llvm.find("@mloptc_div"), std::string::npos);
  EXPECT_EQ(llvm.find("sdiv"), std::string::npos) << "emitted a raw sdiv";
}

TEST(LlvmEmitter, RemainderGoesThroughTheCheckedHelper) {
  const std::string llvm = emitFor("int main() { int d = 0; print(10 % d); return 0; }");

  EXPECT_NE(llvm.find("@mloptc_rem"), std::string::npos);

  // A ToyC '%' must never become a raw srem, which is undefined on a zero
  // divisor. The only srem instructions in the output are the two in the
  // entry point that reduce the exit status mod 256 -- a constant divisor of
  // 256 can neither be zero nor trigger the INT_MIN/-1 case, so those are
  // safe. Counting them pins that down exactly rather than just asserting
  // 'some srem exists somewhere'.
  std::size_t sremCount = 0;
  for (std::size_t at = llvm.find("srem"); at != std::string::npos;
       at = llvm.find("srem", at + 1)) {
    ++sremCount;
  }
  EXPECT_EQ(sremCount, 2U) << "expected srem only in the exit-status reduction:\n" << llvm;
}

TEST(LlvmEmitter, ArrayAccessIsBoundsChecked) {
  const std::string llvm = emitFor("int main() { int a[4]; a[0] = 1; print(a[0]); return 0; }");

  EXPECT_NE(llvm.find("@mloptc_array_addr"), std::string::npos);
  // A raw getelementptr would skip the bounds check the spec requires.
  EXPECT_EQ(llvm.find("getelementptr"), std::string::npos);
}

// =============================================================================
// Structure
// =============================================================================

TEST(LlvmEmitter, EmitsSomethingForEveryCorpusProgram) {
  for (const auto& [name, source] : test::corpus()) {
    SCOPED_TRACE(std::string("program: ") + std::string(name));
    const std::string llvm = emitFor(source, opt::defaultO2Pipeline().passNames);

    EXPECT_FALSE(llvm.empty());
    EXPECT_NE(llvm.find("define i32 @main()"), std::string::npos);
    EXPECT_NE(llvm.find("@toy_main"), std::string::npos);
  }
}

TEST(LlvmEmitter, EveryFunctionEntersAndLeavesAFrame) {
  // mloptc_enter enforces the call-depth limit; mloptc_leave frees the arrays
  // the call allocated. A function missing either would diverge from the VM on
  // array lifetime or on deep recursion.
  const std::string llvm = emitFor(R"(
    int f(int n) { if (n < 1) { return 0; } return f(n - 1); }
    void g() { }
    int main() { g(); return f(3); }
  )");

  EXPECT_NE(llvm.find("@mloptc_enter"), std::string::npos);
  EXPECT_NE(llvm.find("@mloptc_leave"), std::string::npos);
}

TEST(LlvmEmitter, GlobalArraysAreInitialisedBeforeMain) {
  const std::string llvm = emitFor("int g[5]; int main() { print(g[0]); return 0; }");

  EXPECT_NE(llvm.find("@__toyc_init_globals"), std::string::npos);
  EXPECT_NE(llvm.find("@mloptc_array_new"), std::string::npos);
}

TEST(LlvmEmitter, VoidFunctionsEmitVoidReturns) {
  const std::string llvm = emitFor("void f() { print(1); } int main() { f(); return 0; }");

  EXPECT_NE(llvm.find("define void @toy_f()"), std::string::npos);
  EXPECT_NE(llvm.find("ret void"), std::string::npos);
}

TEST(LlvmEmitter, ConstantsAreInlinedRatherThanMaterialised) {
  // Const and Move produce no instructions at all; they are resolved into the
  // operands that use them. Keeps the output readable instead of relying on
  // clang to clean up noise we could just not emit.
  const std::string llvm = emitFor("int main() { print(1 + 2); return 0; }",
                                   opt::defaultO2Pipeline().passNames);
  EXPECT_NE(llvm.find("@mloptc_print(i32 3)"), std::string::npos)
      << "expected the folded constant to be inlined, got:\n"
      << llvm;
}

TEST(LlvmEmitter, ExitStatusIsReducedInTheEmittedCode) {
  // A bug the fuzzer found, on seed 1.
  //
  // The spec defines the exit status as main's return value mod 256. Leaving
  // that to the operating system works on POSIX, which truncates an exit code
  // to 8 bits -- but Windows returns the full 32-bit value, so
  // `return -2147483648` exited with -2147483648 natively while the oracle and
  // the VM both reported 0. The reduction now happens in the emitted code, so
  // the result depends on the spec rather than on the host's libc.
  const std::string llvm = emitFor("int main() { return -2147483648; }");

  EXPECT_NE(llvm.find("srem i32 %r, 256"), std::string::npos)
      << "the entry point does not reduce the exit status:\n"
      << llvm;
}

TEST(LlvmEmitter, OutputIsDeterministic) {
  // The ML tooling compares emitted IR across runs; nondeterminism (from
  // iterating an unordered container, say) would make results irreproducible.
  for (const auto& [name, source] : test::corpus()) {
    SCOPED_TRACE(std::string("program: ") + std::string(name));
    EXPECT_EQ(emitFor(source, opt::defaultO2Pipeline().passNames),
              emitFor(source, opt::defaultO2Pipeline().passNames));
  }
}
