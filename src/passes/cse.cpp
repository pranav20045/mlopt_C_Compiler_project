#include "mloptc/pass.hpp"

#include <map>
#include <tuple>

namespace mloptc::opt {

namespace {

using namespace ir;

/// The identity of a computation: the operation plus its operands.
/// Two instructions with equal keys compute the same value.
struct ExprKey {
  Opcode op;
  ValueId a;
  ValueId b;

  friend bool operator<(const ExprKey& lhs, const ExprKey& rhs) noexcept {
    return std::tie(lhs.op, lhs.a, lhs.b) < std::tie(rhs.op, rhs.a, rhs.b);
  }
};

/// Local common-subexpression elimination.
///
/// If `%3 = add %1, %2` has already been computed in this block and %1 and %2
/// have not changed, a later `%7 = add %1, %2` is replaced by `%7 = move %3`.
/// Copy propagation then forwards %3 to %7's users, and DCE removes the move --
/// another example of passes composing, and of why their order matters.
///
/// **Why only within a basic block.** To reuse a computation from another
/// block, the earlier one must dominate the later one -- otherwise there is a
/// path that reaches the use without having executed the definition, and the
/// register holds garbage. Proving domination needs a dominator tree, which
/// this compiler deliberately does not build. Restricting to a single block
/// makes domination trivially true: earlier in the same block always runs
/// first. "Local" is a real limitation, not a shortcut around correctness.
///
/// **What is eligible.** Only ir::isPure operations: arithmetic and comparison.
/// Deliberately excluded:
///   - Loads, because a store between the two could change the answer. Tracking
///     that needs alias analysis.
///   - Div and Rem, because they can trap. Replacing the second `a / b` with a
///     copy is fine when the first one succeeded, but reasoning about that is
///     more subtle than it looks, and the win is not worth the risk.
///   - Calls, which may do anything at all.
class CommonSubexpressionElimination final : public Pass {
public:
  std::string_view name() const noexcept override { return "cse"; }

  std::string_view description() const noexcept override {
    return "reuse a value already computed earlier in the same block";
  }

  PassStats run(Function& fn, const Module& /*module*/) override {
    PassStats stats;
    stats.passName = std::string(name());

    for (BasicBlock& block : fn.blocks) {
      std::map<ExprKey, ValueId> available;

      for (Instruction& inst : block.instructions) {
        if (!isPure(inst.op) || inst.dest == kNoValue) {
          // Nothing to invalidate: `available` only ever holds pure
          // register-to-register computations, and single assignment means an
          // operand register can never be reassigned. So even a call or a store
          // in the middle of the block cannot invalidate an entry.
          continue;
        }

        ExprKey key{inst.op, inst.a, inst.b};

        // Canonicalise commutative operations so `a + b` and `b + a` match.
        // Unary operations leave b as kNoValue, which sorts consistently.
        if (isCommutative(inst.op) && key.b != kNoValue && key.b < key.a) {
          std::swap(key.a, key.b);
        }

        const auto it = available.find(key);
        if (it != available.end()) {
          inst.op = Opcode::Move;
          inst.a = it->second;
          inst.b = kNoValue;
          ++stats.instructionsRewritten;
          continue;
        }

        available[key] = inst.dest;
      }
    }

    return stats;
  }
};

} // namespace

PassPtr createCommonSubexpressionElimination() {
  return std::make_unique<CommonSubexpressionElimination>();
}

} // namespace mloptc::opt
