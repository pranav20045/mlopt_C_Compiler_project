#include "mloptc/execution.hpp"
#include "mloptc/pass.hpp"

#include <unordered_map>

namespace mloptc::opt {

namespace {

using namespace ir;

/// Constant folding, plus a little algebraic simplification.
///
/// Two things make this simple here. First, every ValueId is assigned exactly
/// once (enforced by the IR verifier), so "register %5 holds the constant 7" is
/// a fact about the whole function, not about one point in it -- no dataflow
/// analysis required. Second, we evaluate using the SAME helpers the backends
/// use (wrapAdd, checkedDiv, ...), so a folded constant is wrapped exactly the
/// way the running program would have wrapped it.
///
/// That second point is the one that bites people. A compiler that folds
/// `2147483647 + 1` using the host's `int` arithmetic has just invoked
/// undefined behaviour in itself, and may produce a constant that disagrees
/// with what the program computes at runtime. Sharing the helpers makes the
/// two agree by construction.
class ConstantFolding final : public Pass {
public:
  std::string_view name() const noexcept override { return "constfold"; }

  std::string_view description() const noexcept override {
    return "evaluate operations on known constants, and simplify algebraic identities";
  }

  PassStats run(Function& fn, const Module& /*module*/) override {
    PassStats stats;
    stats.passName = std::string(name());

    // ValueId -> its constant value, for registers we know are constant.
    std::unordered_map<ValueId, std::int32_t> constants;

    // A single forward pass suffices for straight-line definitions, but a
    // constant can become known only after a later instruction is folded, and
    // definitions can precede uses across blocks in any order. Iterating to a
    // fixed point costs little on functions this size and catches chains that
    // one pass would miss.
    bool changed = true;
    while (changed) {
      changed = false;

      for (BasicBlock& block : fn.blocks) {
        for (Instruction& inst : block.instructions) {
          if (inst.op == Opcode::Const) {
            constants[inst.dest] = inst.imm;
            continue;
          }

          if (tryFold(inst, constants, stats)) {
            changed = true;
          }
        }
      }
    }

    // Folding a branch on a constant turns it into a jump, which can orphan
    // whole blocks.
    if (foldConstantBranches(fn, constants, stats)) {
      const std::size_t before = fn.blocks.size();
      if (removeUnreachableBlocks(fn)) {
        stats.blocksRemoved += static_cast<std::uint32_t>(before - fn.blocks.size());
      }
    }

    computeCfg(fn);
    return stats;
  }

private:
  static const std::int32_t* lookup(const std::unordered_map<ValueId, std::int32_t>& constants,
                                    ValueId id) {
    if (id == kNoValue) {
      return nullptr;
    }
    const auto it = constants.find(id);
    return it == constants.end() ? nullptr : &it->second;
  }

  /// Replaces `inst` with `const value`, recording the new constant.
  static void makeConst(Instruction& inst, std::int32_t value,
                        std::unordered_map<ValueId, std::int32_t>& constants, PassStats& stats) {
    inst.op = Opcode::Const;
    inst.imm = value;
    inst.a = kNoValue;
    inst.b = kNoValue;
    inst.c = kNoValue;
    constants[inst.dest] = value;
    ++stats.instructionsRewritten;
  }

  /// Replaces `inst` with `move src`.
  static void makeMove(Instruction& inst, ValueId src, PassStats& stats) {
    inst.op = Opcode::Move;
    inst.a = src;
    inst.b = kNoValue;
    inst.c = kNoValue;
    ++stats.instructionsRewritten;
  }

  bool tryFold(Instruction& inst, std::unordered_map<ValueId, std::int32_t>& constants,
               PassStats& stats) {
    if (inst.dest == kNoValue || constants.count(inst.dest) != 0) {
      return false; // already folded
    }

    const std::int32_t* a = lookup(constants, inst.a);
    const std::int32_t* b = lookup(constants, inst.b);

    // --- both operands constant: evaluate ---
    if (a != nullptr && b != nullptr) {
      std::int32_t result = 0;
      if (evaluate(inst.op, *a, *b, result)) {
        makeConst(inst, result, constants, stats);
        return true;
      }
      // evaluate() returned false for Div/Rem that would trap. Folding those
      // is NOT allowed: the trap is observable behaviour (exit status 70), so
      // the division has to stay and fail at runtime exactly as before.
      return false;
    }

    if (inst.op == Opcode::Neg && a != nullptr) {
      makeConst(inst, wrapNeg(*a), constants, stats);
      return true;
    }
    if (inst.op == Opcode::Not && a != nullptr) {
      makeConst(inst, *a == 0 ? 1 : 0, constants, stats);
      return true;
    }
    if (inst.op == Opcode::Move && a != nullptr) {
      makeConst(inst, *a, constants, stats);
      return true;
    }

    return trySimplify(inst, a, b, constants, stats);
  }

  /// Algebraic identities with one constant operand.
  ///
  /// Each of these is a place where a plausible-looking "simplification" is
  /// actually wrong, so they are spelled out one at a time rather than written
  /// as a clever table.
  bool trySimplify(Instruction& inst, const std::int32_t* a, const std::int32_t* b,
                   std::unordered_map<ValueId, std::int32_t>& constants, PassStats& stats) {
    switch (inst.op) {
    case Opcode::Add:
      if (a != nullptr && *a == 0) {
        makeMove(inst, inst.b, stats);
        return true;
      }
      if (b != nullptr && *b == 0) {
        makeMove(inst, inst.a, stats);
        return true;
      }
      return false;

    case Opcode::Sub:
      if (b != nullptr && *b == 0) {
        makeMove(inst, inst.a, stats);
        return true;
      }
      // NOT simplified: `0 - x` is not `x`, and folding it to Neg would be
      // correct but is left to keep this function obviously right.
      return false;

    case Opcode::Mul:
      // x * 0 == 0 is safe here ONLY because multiplication cannot trap. The
      // same-shaped rule for division (0 / x == 0) would be wrong: it discards
      // the division-by-zero trap when x is 0.
      if ((a != nullptr && *a == 0) || (b != nullptr && *b == 0)) {
        makeConst(inst, 0, constants, stats);
        return true;
      }
      if (a != nullptr && *a == 1) {
        makeMove(inst, inst.b, stats);
        return true;
      }
      if (b != nullptr && *b == 1) {
        makeMove(inst, inst.a, stats);
        return true;
      }
      return false;

    case Opcode::Div:
      // x / 1 == x is safe: a divisor of 1 can never trap.
      //
      // x / x == 1 is NOT safe (x may be 0), and 0 / x == 0 is NOT safe (same
      // reason). Neither is done.
      if (b != nullptr && *b == 1) {
        makeMove(inst, inst.a, stats);
        return true;
      }
      return false;

    case Opcode::Rem:
      // x % 1 == 0 is safe for the same reason.
      if (b != nullptr && *b == 1) {
        makeConst(inst, 0, constants, stats);
        return true;
      }
      return false;

    default:
      return false;
    }
  }

  /// Returns false if the operation would trap, or is not foldable.
  static bool evaluate(Opcode op, std::int32_t a, std::int32_t b, std::int32_t& out) {
    switch (op) {
    case Opcode::Add:
      out = wrapAdd(a, b);
      return true;
    case Opcode::Sub:
      out = wrapSub(a, b);
      return true;
    case Opcode::Mul:
      out = wrapMul(a, b);
      return true;

    case Opcode::Div: {
      RuntimeStatus status = RuntimeStatus::Ok;
      // Returns false for `1 / 0` and `INT_MIN / -1`, which must keep their
      // runtime trap rather than becoming a compile-time constant.
      return checkedDiv(a, b, out, status);
    }
    case Opcode::Rem: {
      RuntimeStatus status = RuntimeStatus::Ok;
      return checkedRem(a, b, out, status);
    }

    case Opcode::Lt:
      out = a < b ? 1 : 0;
      return true;
    case Opcode::Le:
      out = a <= b ? 1 : 0;
      return true;
    case Opcode::Gt:
      out = a > b ? 1 : 0;
      return true;
    case Opcode::Ge:
      out = a >= b ? 1 : 0;
      return true;
    case Opcode::Eq:
      out = a == b ? 1 : 0;
      return true;
    case Opcode::Ne:
      out = a != b ? 1 : 0;
      return true;

    default:
      return false;
    }
  }

  /// `branch %c ? b1 : b2` where %c is a known constant becomes `jump`.
  ///
  /// This is where constant folding pays for itself beyond arithmetic: it
  /// prunes whole branches of the CFG, which then lets dead-code elimination
  /// remove everything that fed them.
  static bool foldConstantBranches(Function& fn,
                                   const std::unordered_map<ValueId, std::int32_t>& constants,
                                   PassStats& stats) {
    bool any = false;
    for (BasicBlock& block : fn.blocks) {
      if (block.instructions.empty()) {
        continue;
      }
      Instruction& term = block.instructions.back();
      if (term.op != Opcode::Branch) {
        continue;
      }

      const std::int32_t* cond = lookup(constants, term.a);
      if (cond == nullptr) {
        continue;
      }

      term.targetTrue = *cond != 0 ? term.targetTrue : term.targetFalse;
      term.targetFalse = kNoBlock;
      term.op = Opcode::Jump;
      term.a = kNoValue;
      ++stats.instructionsRewritten;
      any = true;
    }
    return any;
  }
};

} // namespace

PassPtr createConstantFolding() {
  return std::make_unique<ConstantFolding>();
}

} // namespace mloptc::opt
