#pragma once

#include "mloptc/ast.hpp"
#include "mloptc/diagnostics.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace mloptc::ir {

/// A virtual register. There is no limit on how many a function may use --
/// register allocation is a backend concern we never reach, since both of our
/// backends (an interpreter and LLVM) take unlimited virtual registers.
using ValueId = std::uint32_t;
using BlockId = std::uint32_t;

inline constexpr ValueId kNoValue = std::numeric_limits<ValueId>::max();
inline constexpr BlockId kNoBlock = std::numeric_limits<BlockId>::max();

/// The complete ToyC instruction set.
///
/// Three-address code: every instruction reads its operands from registers and
/// writes at most one result to a register. Nothing is implicit, nothing is a
/// stack, and no instruction has a hidden operand. That property is what makes
/// the optimization passes tractable -- "what does this instruction depend on"
/// is answered by looking at the instruction itself.
///
/// NOT in SSA form. Local variables live in memory slots and are accessed with
/// LoadLocal/StoreLocal, so there are no phi nodes and no dominator tree. What
/// we *do* guarantee is that every ValueId is assigned exactly once (see
/// IrVerifier), which is the single property the optimization passes actually
/// need: it makes "the value in register r" a well-defined, position-
/// independent thing, so constant folding and copy propagation are valid
/// across the whole function rather than only within a block.
enum class Opcode : std::uint8_t {
  // --- value production ---
  Const, ///< dest = imm
  Move,  ///< dest = a

  // --- arithmetic (wrapping; see docs/spec.md 7.1) ---
  Add,
  Sub,
  Mul,
  Div, ///< may trap: division by zero, INT_MIN / -1
  Rem, ///< may trap

  // --- comparison; each yields 0 or 1 ---
  Lt,
  Le,
  Gt,
  Ge,
  Eq,
  Ne,

  // --- unary ---
  Neg,
  Not,

  // --- scalar memory ---
  LoadLocal,   ///< dest = frame[imm]
  StoreLocal,  ///< frame[imm] = a
  LoadGlobal,  ///< dest = globals[imm]
  StoreGlobal, ///< globals[imm] = a

  // --- arrays ---
  Alloca,     ///< dest = a fresh zeroed array of `imm` elements (a handle)
  ArrayLoad,  ///< dest = a[b]; may trap on a bad index
  ArrayStore, ///< a[b] = c; may trap

  // --- effects ---
  Call,  ///< dest = funcIndex(args...)
  Print, ///< writes a, then a newline

  // --- terminators; every block ends with exactly one ---
  Jump,   ///< -> targetTrue
  Branch, ///< a != 0 ? targetTrue : targetFalse
  Return, ///< returns a, or nothing if a == kNoValue
};

[[nodiscard]] std::string_view opcodeName(Opcode op) noexcept;

/// True for instructions that end a basic block.
[[nodiscard]] bool isTerminator(Opcode op) noexcept;

/// True if the instruction writes a register.
[[nodiscard]] bool hasResult(Opcode op) noexcept;

/// True if removing the instruction could change observable behaviour.
///
/// This is the predicate dead-code elimination consults, and it has a subtlety
/// that is easy to get wrong: **Div, Rem and ArrayLoad produce a value AND may
/// trap.** If `x = a / b` has an unused result, deleting it looks like a free
/// win -- but if `b` was zero, the program was supposed to exit with status 70,
/// and the optimized program would run on instead. Removing a trap is just as
/// much a miscompilation as computing the wrong number, so these count as
/// having side effects even though they look pure.
[[nodiscard]] bool mayHaveSideEffects(Opcode op) noexcept;

/// True for instructions whose result depends only on their operands, with no
/// memory access and no trap. These are the only ones CSE may reuse.
[[nodiscard]] bool isPure(Opcode op) noexcept;

/// True if the opcode is commutative, so CSE can match `a + b` against `b + a`.
[[nodiscard]] bool isCommutative(Opcode op) noexcept;

struct Instruction {
  Opcode op = Opcode::Const;
  ValueId dest = kNoValue;

  /// Register operands. How many are used depends on the opcode; see the
  /// comments on each Opcode above.
  ValueId a = kNoValue;
  ValueId b = kNoValue;
  ValueId c = kNoValue;

  /// Immediate: the constant for Const, the slot index for Load/Store, the
  /// element count for Alloca.
  std::int32_t imm = 0;

  /// Call only.
  std::uint32_t funcIndex = 0;
  std::vector<ValueId> args;

  /// Jump/Branch only.
  BlockId targetTrue = kNoBlock;
  BlockId targetFalse = kNoBlock;

  SourceLoc loc;

  /// Calls every register operand this instruction READS. Centralising this is
  /// what keeps the passes honest: adding an operand to an opcode without
  /// updating this function would silently break every use-analysis at once.
  template <typename F> void forEachUse(F&& fn) const {
    switch (op) {
    case Opcode::Const:
    case Opcode::Jump:
    case Opcode::Alloca:
    case Opcode::LoadLocal:
    case Opcode::LoadGlobal:
      return;

    case Opcode::Move:
    case Opcode::Neg:
    case Opcode::Not:
    case Opcode::StoreLocal:
    case Opcode::StoreGlobal:
    case Opcode::Print:
    case Opcode::Branch:
      fn(a);
      return;

    case Opcode::Return:
      if (a != kNoValue) {
        fn(a);
      }
      return;

    case Opcode::Add:
    case Opcode::Sub:
    case Opcode::Mul:
    case Opcode::Div:
    case Opcode::Rem:
    case Opcode::Lt:
    case Opcode::Le:
    case Opcode::Gt:
    case Opcode::Ge:
    case Opcode::Eq:
    case Opcode::Ne:
    case Opcode::ArrayLoad:
      fn(a);
      fn(b);
      return;

    case Opcode::ArrayStore:
      fn(a);
      fn(b);
      fn(c);
      return;

    case Opcode::Call:
      for (const ValueId arg : args) {
        fn(arg);
      }
      return;
    }
  }

  /// The mutable counterpart of forEachUse, used by passes that rewrite
  /// operands (copy propagation, constant folding).
  template <typename F> void forEachUseMut(F&& fn) {
    switch (op) {
    case Opcode::Const:
    case Opcode::Jump:
    case Opcode::Alloca:
    case Opcode::LoadLocal:
    case Opcode::LoadGlobal:
      return;

    case Opcode::Move:
    case Opcode::Neg:
    case Opcode::Not:
    case Opcode::StoreLocal:
    case Opcode::StoreGlobal:
    case Opcode::Print:
    case Opcode::Branch:
      fn(a);
      return;

    case Opcode::Return:
      if (a != kNoValue) {
        fn(a);
      }
      return;

    case Opcode::Add:
    case Opcode::Sub:
    case Opcode::Mul:
    case Opcode::Div:
    case Opcode::Rem:
    case Opcode::Lt:
    case Opcode::Le:
    case Opcode::Gt:
    case Opcode::Ge:
    case Opcode::Eq:
    case Opcode::Ne:
    case Opcode::ArrayLoad:
      fn(a);
      fn(b);
      return;

    case Opcode::ArrayStore:
      fn(a);
      fn(b);
      fn(c);
      return;

    case Opcode::Call:
      for (ValueId& arg : args) {
        fn(arg);
      }
      return;
    }
  }
};

struct BasicBlock {
  BlockId id = kNoBlock;
  std::string label;
  std::vector<Instruction> instructions;

  /// Maintained by `computeCfg`, not by hand. Any pass that changes a
  /// terminator must call computeCfg again (the pass manager does).
  std::vector<BlockId> predecessors;
  std::vector<BlockId> successors;

  [[nodiscard]] bool empty() const noexcept { return instructions.empty(); }

  [[nodiscard]] const Instruction* terminator() const noexcept {
    if (instructions.empty() || !isTerminator(instructions.back().op)) {
      return nullptr;
    }
    return &instructions.back();
  }
};

struct Function {
  std::string name;
  Type returnType = Type::Int;
  std::uint32_t numParams = 0;

  /// Number of scalar slots in the frame. Includes parameters, every declared
  /// local, and the temporaries lowering introduces for short-circuit results.
  std::uint32_t frameSize = 0;

  std::vector<BasicBlock> blocks;
  BlockId entry = 0;

  /// One past the highest ValueId used. Passes that create registers take the
  /// next one from here.
  ValueId nextValue = 0;

  [[nodiscard]] std::size_t instructionCount() const noexcept;
};

struct Module {
  /// Carried over verbatim from the AST; the backends initialise storage from
  /// this before entering main.
  std::vector<GlobalVarDecl> globals;
  std::vector<Function> functions;
  std::uint32_t mainIndex = 0;
};

/// Recomputes every block's predecessor and successor lists from its
/// terminator. Cheap, and idempotent, so passes call it rather than trying to
/// keep the CFG correct incrementally -- incremental updates are where CFG bugs
/// come from.
void computeCfg(Function& fn);

/// Removes blocks unreachable from the entry block, renumbering the rest.
///
/// Passes that fold a conditional branch into an unconditional jump leave
/// blocks behind with no way in. They are harmless to execute (nothing reaches
/// them) but they distort every statistic the ML model trains on, so they are
/// swept up here.
bool removeUnreachableBlocks(Function& fn);

/// Renders a module as readable text, the format used by --emit-ir, by tests,
/// and when debugging a pass that has gone wrong.
[[nodiscard]] std::string printModule(const Module& module);
[[nodiscard]] std::string printFunction(const Function& fn);

} // namespace mloptc::ir
