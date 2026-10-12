#include "mloptc/driver.hpp"
#include "mloptc/interpreter.hpp"
#include "mloptc/ir_verifier.hpp"
#include "mloptc/lowering.hpp"
#include "mloptc/pass_manager.hpp"
#include "mloptc/vm.hpp"

#include "corpus.hpp"

#include <algorithm>
#include <gtest/gtest.h>
#include <string>
#include <vector>

using namespace mloptc;

namespace {

struct Compiled {
  Program ast;
  ir::Module module;
};

bool compile(std::string_view source, Compiled& out) {
  DiagnosticEngine diags;
  auto program = compileFrontend(source, diags);
  if (!program) {
    std::string messages;
    for (const Diagnostic& d : diags.all()) {
      messages += d.message + "; ";
    }
    ADD_FAILURE() << "front end rejected the program: " << messages;
    return false;
  }
  out.ast = std::move(*program);
  out.module = lower(out.ast);
  return true;
}

/// Optimizes with the given passes and returns the result of running the
/// optimized IR.
ExecutionResult runOptimized(const Compiled& compiled, const std::vector<std::string>& passes,
                             opt::PipelineStats& stats) {
  ir::Module copy = compiled.module;
  std::string error;
  if (!opt::optimizeModule(copy, passes, stats, error)) {
    ADD_FAILURE() << "optimization failed: " << error;
    return {};
  }
  return execute(copy);
}

} // namespace

// =============================================================================
// THE central correctness claim for the optimizer
// =============================================================================

TEST(Passes, EveryIndividualPassPreservesBehaviour) {
  for (const std::string& passName : opt::allPassNames()) {
    for (const auto& [name, source] : test::corpus()) {
      SCOPED_TRACE("pass: " + passName + ", program: " + std::string(name));

      Compiled compiled;
      if (!compile(source, compiled)) {
        continue;
      }

      const ExecutionResult oracle = interpret(compiled.ast);
      opt::PipelineStats stats;
      const ExecutionResult optimized = runOptimized(compiled, {passName}, stats);

      EXPECT_EQ(oracle.output, optimized.output);
      EXPECT_EQ(oracle.exitStatus, optimized.exitStatus);
      EXPECT_EQ(oracle.status, optimized.status) << "trap behaviour changed";
    }
  }
}

TEST(Passes, DefaultO2PipelinePreservesBehaviour) {
  const opt::Pipeline pipeline = opt::defaultO2Pipeline();

  for (const auto& [name, source] : test::corpus()) {
    SCOPED_TRACE(std::string("program: ") + std::string(name));

    Compiled compiled;
    if (!compile(source, compiled)) {
      continue;
    }

    const ExecutionResult oracle = interpret(compiled.ast);
    opt::PipelineStats stats;
    const ExecutionResult optimized = runOptimized(compiled, pipeline.passNames, stats);

    EXPECT_TRUE(oracle.observablyEquals(optimized));
    EXPECT_EQ(oracle.status, optimized.status);
  }
}

TEST(Passes, EveryPermutationOfPassesPreservesBehaviour) {
  // 5 passes => 120 orderings, each run over the whole corpus. This is the test
  // that justifies letting an ML model pick an arbitrary pass order: if any
  // ordering could break a program, a learned scheduler would eventually find
  // it. Correctness must not depend on the order being sensible.
  std::vector<std::string> passes = opt::allPassNames();
  std::sort(passes.begin(), passes.end());

  int orderingsChecked = 0;
  do {
    for (const auto& [name, source] : test::corpus()) {
      Compiled compiled;
      if (!compile(source, compiled)) {
        continue;
      }

      const ExecutionResult oracle = interpret(compiled.ast);
      opt::PipelineStats stats;
      const ExecutionResult optimized = runOptimized(compiled, passes, stats);

      if (!oracle.observablyEquals(optimized) || oracle.status != optimized.status) {
        std::string order;
        for (const std::string& p : passes) {
          order += p + " ";
        }
        ADD_FAILURE() << "ordering [" << order << "] changed behaviour of " << name;
      }
    }
    ++orderingsChecked;
  } while (std::next_permutation(passes.begin(), passes.end()));

  EXPECT_EQ(orderingsChecked, 120);
}

TEST(Passes, RepeatedApplicationIsStable) {
  // Running a pipeline twice must not change the result of running it once.
  // A pass that keeps "optimizing" forever would make the ML cost signal
  // depend on how many times a sequence happened to be repeated.
  const opt::Pipeline pipeline = opt::defaultO2Pipeline();

  for (const auto& [name, source] : test::corpus()) {
    SCOPED_TRACE(std::string("program: ") + std::string(name));

    Compiled compiled;
    if (!compile(source, compiled)) {
      continue;
    }

    ir::Module once = compiled.module;
    ir::Module twice = compiled.module;
    opt::PipelineStats stats;
    std::string error;

    ASSERT_TRUE(opt::optimizeModule(once, pipeline.passNames, stats, error)) << error;
    ASSERT_TRUE(opt::optimizeModule(twice, pipeline.passNames, stats, error)) << error;
    ASSERT_TRUE(opt::optimizeModule(twice, pipeline.passNames, stats, error)) << error;

    EXPECT_EQ(ir::printModule(once), ir::printModule(twice))
        << "pipeline is not idempotent on " << name;
  }
}

TEST(Passes, OptimizedIrAlwaysVerifies) {
  const opt::Pipeline pipeline = opt::defaultO2Pipeline();

  for (const auto& [name, source] : test::corpus()) {
    SCOPED_TRACE(std::string("program: ") + std::string(name));

    Compiled compiled;
    if (!compile(source, compiled)) {
      continue;
    }

    ir::Module module = compiled.module;
    opt::PipelineStats stats;
    std::string error;
    ASSERT_TRUE(opt::optimizeModule(module, pipeline.passNames, stats, error)) << error;

    const ir::VerificationResult result = ir::verify(module);
    EXPECT_TRUE(result.ok()) << result.message();
  }
}

// =============================================================================
// The traps that must survive optimization
// =============================================================================

TEST(Passes, DceDoesNotRemoveATrappingDivision) {
  // The headline DCE trap. The division's result is never read, so a naive
  // implementation deletes it -- and the program stops exiting with status 70.
  Compiled compiled;
  ASSERT_TRUE(compile(R"(
    int main() {
      int d = 0;
      print(1);
      int neverRead = 10 / d;
      print(2);
      return 0;
    }
  )",
                      compiled));

  opt::PipelineStats stats;
  const ExecutionResult optimized = runOptimized(compiled, {"dce", "constfold", "dce"}, stats);

  EXPECT_EQ(optimized.output, "1\n") << "the trap was optimized away";
  EXPECT_EQ(optimized.exitStatus, 70);
  EXPECT_EQ(optimized.status, RuntimeStatus::DivisionByZero);
}

TEST(Passes, DceDoesNotRemoveATrappingArrayLoad) {
  Compiled compiled;
  ASSERT_TRUE(compile(R"(
    int main() {
      int a[3];
      print(1);
      int neverRead = a[99];
      print(2);
      return 0;
    }
  )",
                      compiled));

  opt::PipelineStats stats;
  const ExecutionResult optimized = runOptimized(compiled, {"dce"}, stats);

  EXPECT_EQ(optimized.output, "1\n");
  EXPECT_EQ(optimized.status, RuntimeStatus::IndexOutOfBounds);
}

TEST(Passes, ConstantFoldingDoesNotFoldATrappingDivision) {
  // Both operands are compile-time constants, so this is exactly the case a
  // folder is tempted to evaluate -- and evaluating it would either crash the
  // compiler or silently produce a wrong constant.
  Compiled compiled;
  ASSERT_TRUE(compile("int main() { print(1); print(10 / 0); return 0; }", compiled));

  opt::PipelineStats stats;
  const ExecutionResult optimized = runOptimized(compiled, {"constfold"}, stats);

  EXPECT_EQ(optimized.output, "1\n");
  EXPECT_EQ(optimized.status, RuntimeStatus::DivisionByZero);
}

TEST(Passes, ConstantFoldingDoesNotFoldIntMinDividedByMinusOne) {
  Compiled compiled;
  ASSERT_TRUE(compile("int main() { print(-2147483648 / -1); return 0; }", compiled));

  opt::PipelineStats stats;
  const ExecutionResult optimized = runOptimized(compiled, {"constfold"}, stats);

  EXPECT_EQ(optimized.status, RuntimeStatus::DivisionOverflow);
}

TEST(Passes, ConstantFoldingWrapsLikeTheRuntime) {
  // If the folder used host int arithmetic, this would be UB inside our own
  // compiler and could produce a constant that disagrees with the VM.
  Compiled compiled;
  ASSERT_TRUE(
      compile("int main() { print(2147483647 + 1); print(2147483647 * 3); return 0; }", compiled));

  const ExecutionResult oracle = interpret(compiled.ast);
  opt::PipelineStats stats;
  const ExecutionResult optimized = runOptimized(compiled, {"constfold"}, stats);

  EXPECT_EQ(oracle.output, optimized.output);
  EXPECT_EQ(optimized.output, "-2147483648\n2147483645\n");
}

TEST(Passes, MultiplyByZeroIsFoldedButDivideIsNot) {
  // x * 0 folds to 0 because multiplication cannot trap.
  // 0 / x does NOT fold to 0, because x may be zero.
  Compiled compiled;
  ASSERT_TRUE(compile(R"(
    int main() {
      int z = 0;
      print(z * 0);
      print(0 / z);
      return 0;
    }
  )",
                      compiled));

  opt::PipelineStats stats;
  const ExecutionResult optimized = runOptimized(compiled, {"constfold", "dce"}, stats);

  EXPECT_EQ(optimized.output, "0\n");
  EXPECT_EQ(optimized.status, RuntimeStatus::DivisionByZero);
}

TEST(Passes, ShortCircuitSemanticsSurviveOptimization) {
  Compiled compiled;
  ASSERT_TRUE(compile(R"(
    int side(int n) { print(n); return n; }
    int main() {
      int d = 0;
      if (d != 0 && 100 / d > 0) { print(999); }
      if (0 && side(1)) { print(998); }
      if (1 || side(2)) { print(3); }
      return 0;
    }
  )",
                      compiled));

  const ExecutionResult oracle = interpret(compiled.ast);
  opt::PipelineStats stats;
  const ExecutionResult optimized =
      runOptimized(compiled, opt::defaultO2Pipeline().passNames, stats);

  EXPECT_EQ(oracle.output, optimized.output);
  EXPECT_TRUE(optimized.ok()) << "a guarded division trapped after optimization";
}

// =============================================================================
// The passes actually do something
// =============================================================================

TEST(Passes, O2ReducesInstructionCount) {
  Compiled compiled;
  ASSERT_TRUE(compile(R"(
    int main() {
      int a = 2 + 3 * 4;
      int b = a;
      int c = b;
      int unused = c * 0 + 17;
      print(c);
      return 0;
    }
  )",
                      compiled));

  opt::PipelineStats stats;
  const ExecutionResult optimized =
      runOptimized(compiled, opt::defaultO2Pipeline().passNames, stats);

  EXPECT_EQ(optimized.output, "14\n");
  EXPECT_LT(stats.instructionsAfter, stats.instructionsBefore) << "the pipeline removed nothing";
}

TEST(Passes, O2ReducesDynamicInstructionsOnTheCorpus) {
  // The cost signal the ML scheduler optimizes must actually respond to
  // optimization, or there is nothing to learn.
  int improved = 0;
  int total = 0;

  for (const auto& [name, source] : test::corpus()) {
    Compiled compiled;
    if (!compile(source, compiled)) {
      continue;
    }

    const ExecutionResult unoptimized = execute(compiled.module);
    opt::PipelineStats stats;
    const ExecutionResult optimized =
        runOptimized(compiled, opt::defaultO2Pipeline().passNames, stats);

    ++total;
    if (optimized.dynamicInstructions < unoptimized.dynamicInstructions) {
      ++improved;
    }
    EXPECT_LE(optimized.dynamicInstructions, unoptimized.dynamicInstructions)
        << name << " got SLOWER after -O2";
  }

  EXPECT_GT(improved, total / 2) << "-O2 improved only " << improved << " of " << total;
}

TEST(Passes, ConstantFoldingFoldsAChain) {
  Compiled compiled;
  ASSERT_TRUE(compile("int main() { print(2 + 3 * 4 - 1); return 0; }", compiled));

  opt::PipelineStats stats;
  const ExecutionResult optimized = runOptimized(compiled, {"constfold", "dce"}, stats);

  EXPECT_EQ(optimized.output, "13\n");
  EXPECT_LT(stats.instructionsAfter, stats.instructionsBefore);
}

TEST(Passes, CseRewritesARepeatedExpression) {
  // Note the pipeline: cse ALONE finds nothing here, and that is not a bug.
  // Each read of x and y is a separate LoadLocal producing a fresh register,
  // so the two multiplies have different operands until loadelim and copyprop
  // have run. This is the central limitation of a non-SSA IR, and the reason
  // loadelim exists.
  Compiled compiled;
  ASSERT_TRUE(compile(R"(
    int f(int x, int y) {
      int p = x * y;
      int q = x * y;
      return p + q;
    }
    int main() { print(f(3, 4)); return 0; }
  )",
                      compiled));

  opt::PipelineStats stats;
  const ExecutionResult optimized = runOptimized(compiled, {"loadelim", "copyprop", "cse"}, stats);

  EXPECT_EQ(optimized.output, "24\n");
  bool cseDidWork = false;
  for (const opt::PassStats& s : stats.perPass) {
    if (s.passName == "cse" && s.instructionsRewritten > 0) {
      cseDidWork = true;
    }
  }
  EXPECT_TRUE(cseDidWork) << "cse found no redundancy in an obviously redundant program";
}

TEST(Passes, CseMatchesCommutativeOperandsInEitherOrder) {
  Compiled compiled;
  ASSERT_TRUE(compile(R"(
    int f(int x, int y) { return (x + y) + (y + x); }
    int main() { print(f(3, 4)); return 0; }
  )",
                      compiled));

  opt::PipelineStats stats;
  const ExecutionResult optimized = runOptimized(compiled, {"loadelim", "copyprop", "cse"}, stats);
  EXPECT_EQ(optimized.output, "14\n");
}

TEST(Passes, CopyPropagationCollapsesAChain) {
  Compiled compiled;
  ASSERT_TRUE(compile(R"(
    int f(int x) { int a = x; int b = a; int c = b; return c; }
    int main() { print(f(9)); return 0; }
  )",
                      compiled));

  opt::PipelineStats stats;
  const ExecutionResult optimized = runOptimized(compiled, {"cse", "copyprop", "dce"}, stats);
  EXPECT_EQ(optimized.output, "9\n");
}

TEST(Passes, ConstantBranchIsFoldedAndDeadBlocksRemoved) {
  Compiled compiled;
  ASSERT_TRUE(compile(R"(
    int main() {
      if (1 < 2) { print(1); } else { print(2); }
      return 0;
    }
  )",
                      compiled));

  const std::size_t blocksBefore = compiled.module.functions[0].blocks.size();

  ir::Module module = compiled.module;
  opt::PipelineStats stats;
  std::string error;
  ASSERT_TRUE(opt::optimizeModule(module, {"constfold", "dce"}, stats, error)) << error;

  EXPECT_EQ(execute(module).output, "1\n");
  EXPECT_LT(module.functions[0].blocks.size(), blocksBefore)
      << "the unreachable else branch was not removed";
}

// =============================================================================
// Pass manager behaviour
// =============================================================================

TEST(PassManager, RejectsUnknownPassName) {
  Compiled compiled;
  ASSERT_TRUE(compile("int main() { return 0; }", compiled));

  opt::PipelineStats stats;
  std::string error;
  EXPECT_FALSE(opt::optimizeModule(compiled.module, {"nosuchpass"}, stats, error));
  EXPECT_NE(error.find("nosuchpass"), std::string::npos);
}

TEST(PassManager, EmptyPipelineIsANoOp) {
  Compiled compiled;
  ASSERT_TRUE(compile("int main() { print(1 + 1); return 0; }", compiled));

  const std::string before = ir::printModule(compiled.module);

  opt::PipelineStats stats;
  std::string error;
  ASSERT_TRUE(opt::optimizeModule(compiled.module, {}, stats, error)) << error;

  EXPECT_EQ(before, ir::printModule(compiled.module));
  EXPECT_EQ(stats.instructionsBefore, stats.instructionsAfter);
}

TEST(PassManager, DetectsAPassThatCorruptsTheIr) {
  // Proves the verifier-after-every-pass safety net is actually wired up: we
  // corrupt the IR by hand and confirm the manager refuses to proceed.
  //
  // Verification is enabled EXPLICITLY rather than relying on the build type.
  // PassManagerOptions defaults it off under NDEBUG -- which is the right
  // default, since verifying after every pass costs real time in a release
  // compiler -- but a test that inherits that default silently stops testing
  // anything in a release build. This one failed in `ctest` on the release
  // preset for exactly that reason.
  Compiled compiled;
  ASSERT_TRUE(compile("int main() { print(1); return 0; }", compiled));

  compiled.module.functions[0].blocks[0].instructions.back().op = ir::Opcode::Print;

  opt::PassManagerOptions options;
  options.verifyAfterEachPass = true;
  opt::PassManager manager(options);

  opt::PipelineStats stats;
  std::string error;
  EXPECT_FALSE(manager.run(compiled.module, std::vector<std::string>{"dce"}, stats, error));
  EXPECT_NE(error.find("invalid"), std::string::npos) << error;
}

TEST(PassManager, SkipsVerificationWhenAskedTo) {
  // The other half of the contract: a release build really does skip
  // verification, so the cost is not silently being paid.
  Compiled compiled;
  ASSERT_TRUE(compile("int main() { print(1); return 0; }", compiled));

  compiled.module.functions[0].blocks[0].instructions.back().op = ir::Opcode::Print;

  opt::PassManagerOptions options;
  options.verifyAfterEachPass = false;
  opt::PassManager manager(options);

  opt::PipelineStats stats;
  std::string error;
  EXPECT_TRUE(manager.run(compiled.module, std::vector<std::string>{"dce"}, stats, error))
      << "verification ran even though it was disabled";
}

TEST(PassManager, ReportsPerPassStatistics) {
  Compiled compiled;
  ASSERT_TRUE(compile("int main() { int unused = 1 + 2; print(5); return 0; }", compiled));

  opt::PipelineStats stats;
  std::string error;
  ASSERT_TRUE(opt::optimizeModule(compiled.module, {"constfold", "dce"}, stats, error)) << error;

  ASSERT_EQ(stats.perPass.size(), 2U);
  EXPECT_EQ(stats.perPass[0].passName, "constfold");
  EXPECT_EQ(stats.perPass[1].passName, "dce");
  EXPECT_FALSE(stats.report().empty());
}
