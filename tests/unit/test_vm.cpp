#include "corpus.hpp"

#include "mloptc/driver.hpp"
#include "mloptc/interpreter.hpp"
#include "mloptc/ir_verifier.hpp"
#include "mloptc/lowering.hpp"
#include "mloptc/vm.hpp"

#include <gtest/gtest.h>

using namespace mloptc;

namespace {

struct Compiled {
  Program ast;
  ir::Module module;
};

/// Compiles to IR, asserting the front end accepted the program and that the
/// resulting IR is structurally valid.
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

  const ir::VerificationResult verification = ir::verify(out.module);
  if (!verification.ok()) {
    ADD_FAILURE() << "lowering produced invalid IR:\n"
                  << verification.message() << "\n"
                  << ir::printModule(out.module);
    return false;
  }
  return true;
}

} // namespace

// =============================================================================
// Lowering produces valid IR
// =============================================================================

TEST(Lowering, ProducesValidIrForWholeCorpus) {
  for (const auto& [name, source] : test::corpus()) {
    SCOPED_TRACE(std::string("program: ") + std::string(name));
    Compiled compiled;
    EXPECT_TRUE(compile(source, compiled));
  }
}

TEST(Lowering, EveryBlockEndsInATerminator) {
  for (const auto& [name, source] : test::corpus()) {
    SCOPED_TRACE(std::string("program: ") + std::string(name));
    Compiled compiled;
    if (!compile(source, compiled)) {
      continue;
    }
    for (const ir::Function& fn : compiled.module.functions) {
      for (const ir::BasicBlock& block : fn.blocks) {
        EXPECT_NE(block.terminator(), nullptr)
            << fn.name << " block b" << block.id << " has no terminator";
      }
    }
  }
}

TEST(Lowering, EveryBlockIsReachable) {
  for (const auto& [name, source] : test::corpus()) {
    SCOPED_TRACE(std::string("program: ") + std::string(name));
    Compiled compiled;
    if (!compile(source, compiled)) {
      continue;
    }
    for (ir::Function& fn : compiled.module.functions) {
      // Lowering already sweeps these; if any remain, something regressed.
      EXPECT_FALSE(ir::removeUnreachableBlocks(fn))
          << fn.name << " still had unreachable blocks after lowering";
    }
  }
}

// =============================================================================
// THE central claim: the VM agrees with the oracle
// =============================================================================

TEST(Differential, VmMatchesOracleOnWholeCorpus) {
  for (const auto& [name, source] : test::corpus()) {
    SCOPED_TRACE(std::string("program: ") + std::string(name));

    Compiled compiled;
    if (!compile(source, compiled)) {
      continue;
    }

    const ExecutionResult oracle = interpret(compiled.ast);
    const ExecutionResult vm = execute(compiled.module);

    EXPECT_EQ(oracle.output, vm.output) << "stdout differs for " << name;
    EXPECT_EQ(oracle.exitStatus, vm.exitStatus) << "exit status differs for " << name;
    EXPECT_TRUE(oracle.observablyEquals(vm));
  }
}

TEST(Differential, TrapsAgreeBetweenOracleAndVm) {
  // The trapping programs deserve their own assertion: it is not enough that
  // both paths fail, they must fail for the SAME reason and after producing
  // the same output.
  for (const auto& [name, source] : test::corpus()) {
    SCOPED_TRACE(std::string("program: ") + std::string(name));

    Compiled compiled;
    if (!compile(source, compiled)) {
      continue;
    }

    const ExecutionResult oracle = interpret(compiled.ast);
    const ExecutionResult vm = execute(compiled.module);

    EXPECT_EQ(oracle.status, vm.status) << "trap reason differs for " << name;
  }
}

// =============================================================================
// Instruction counting
// =============================================================================

TEST(Vm, CountsInstructions) {
  Compiled compiled;
  ASSERT_TRUE(compile("int main() { print(1); return 0; }", compiled));

  const ExecutionResult result = execute(compiled.module);
  EXPECT_TRUE(result.ok());
  EXPECT_GT(result.dynamicInstructions, 0U);
}

TEST(Vm, InstructionCountGrowsWithWork) {
  // The cost signal the ML model trains on must actually track work done,
  // otherwise the whole optimization target is meaningless.
  Compiled small;
  Compiled large;
  ASSERT_TRUE(compile("int main() { for (int i = 0; i < 10; i = i + 1) { print(i); } return 0; }",
                      small));
  ASSERT_TRUE(compile(
      "int main() { for (int i = 0; i < 100; i = i + 1) { print(i); } return 0; }", large));

  EXPECT_LT(execute(small.module).dynamicInstructions,
            execute(large.module).dynamicInstructions);
}

TEST(Vm, StepLimitIsEnforced) {
  Compiled compiled;
  ASSERT_TRUE(compile("int main() { while (1) { } return 0; }", compiled));

  VmOptions options;
  options.maxInstructions = 10000;
  const ExecutionResult result = execute(compiled.module, options);

  EXPECT_EQ(result.status, RuntimeStatus::StepLimitExceeded);
}

// =============================================================================
// IR verifier catches what it is supposed to
// =============================================================================

TEST(IrVerifier, RejectsBlockWithoutTerminator) {
  Compiled compiled;
  ASSERT_TRUE(compile("int main() { print(1); return 0; }", compiled));

  compiled.module.functions[0].blocks[0].instructions.pop_back();
  EXPECT_FALSE(ir::verify(compiled.module).ok());
}

TEST(IrVerifier, RejectsJumpToNonexistentBlock) {
  Compiled compiled;
  ASSERT_TRUE(compile("int main() { if (1) { print(1); } return 0; }", compiled));

  ir::Function& fn = compiled.module.functions[0];
  for (ir::BasicBlock& block : fn.blocks) {
    if (!block.instructions.empty() && block.instructions.back().op == ir::Opcode::Branch) {
      block.instructions.back().targetTrue = 9999;
      break;
    }
  }
  EXPECT_FALSE(ir::verify(compiled.module).ok());
}

TEST(IrVerifier, RejectsValueDefinedTwice) {
  // The pseudo-SSA property the passes depend on.
  Compiled compiled;
  ASSERT_TRUE(compile("int main() { print(1 + 2); return 0; }", compiled));

  ir::Function& fn = compiled.module.functions[0];
  ir::BasicBlock& entry = fn.blocks[0];
  ASSERT_GE(entry.instructions.size(), 2U);
  entry.instructions[1].dest = entry.instructions[0].dest;

  EXPECT_FALSE(ir::verify(compiled.module).ok());
}

TEST(IrVerifier, RejectsUseBeforeDefinition) {
  Compiled compiled;
  ASSERT_TRUE(compile("int main() { print(1 + 2); return 0; }", compiled));

  ir::Function& fn = compiled.module.functions[0];
  fn.blocks[0].instructions[0].dest = fn.nextValue;
  ++fn.nextValue;

  EXPECT_FALSE(ir::verify(compiled.module).ok());
}

TEST(IrVerifier, RejectsOutOfRangeLocalSlot) {
  Compiled compiled;
  ASSERT_TRUE(compile("int main() { int x = 1; print(x); return 0; }", compiled));

  ir::Function& fn = compiled.module.functions[0];
  for (ir::BasicBlock& block : fn.blocks) {
    for (ir::Instruction& inst : block.instructions) {
      if (inst.op == ir::Opcode::StoreLocal) {
        inst.imm = static_cast<std::int32_t>(fn.frameSize) + 10;
      }
    }
  }
  EXPECT_FALSE(ir::verify(compiled.module).ok());
}

TEST(IrVerifier, AcceptsWellFormedCorpus) {
  for (const auto& [name, source] : test::corpus()) {
    SCOPED_TRACE(std::string("program: ") + std::string(name));
    Compiled compiled;
    if (!compile(source, compiled)) {
      continue;
    }
    const auto result = ir::verify(compiled.module);
    EXPECT_TRUE(result.ok()) << result.message();
  }
}

// =============================================================================
// Structure of the lowered IR
// =============================================================================

TEST(Lowering, ShortCircuitCreatesBranches) {
  Compiled compiled;
  ASSERT_TRUE(compile("int f() { return 1; } int main() { print(0 && f()); return 0; }",
                      compiled));

  const ir::Function& main = compiled.module.functions[1];
  EXPECT_GT(main.blocks.size(), 1U) << "&& should have produced control flow";
}

TEST(Lowering, ForLoopHasSeparateUpdateBlock) {
  // The update must be its own block so `continue` can target it.
  Compiled compiled;
  ASSERT_TRUE(compile("int main() { for (int i = 0; i < 3; i = i + 1) { print(i); } return 0; }",
                      compiled));

  const ir::Function& main = compiled.module.functions[0];
  bool foundUpdate = false;
  for (const ir::BasicBlock& block : main.blocks) {
    if (block.label == "for.update") {
      foundUpdate = true;
    }
  }
  EXPECT_TRUE(foundUpdate);
}

TEST(Lowering, UnreachableCodeAfterReturnIsDropped) {
  Compiled compiled;
  ASSERT_TRUE(compile("int main() { return 1; print(999); }", compiled));

  const ExecutionResult result = execute(compiled.module);
  EXPECT_EQ(result.output, "");
  EXPECT_EQ(result.exitStatus, 1);
}
