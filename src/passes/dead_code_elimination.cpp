#include "mloptc/pass.hpp"

#include <vector>

namespace mloptc::opt {

namespace {

using namespace ir;

/// Dead-code elimination: remove instructions whose result nobody reads and
/// which have no side effects.
///
/// The whole pass hinges on getting "no side effects" right, and the trap below
/// is the one that matters:
///
///     int d = 0;
///     int unused = 10 / d;   // result never read
///     print(1);
///
/// The division's result is dead, so a naive DCE deletes it -- and a program
/// that should have exited with status 70 now prints 1 and exits 0. Removing a
/// trap is a miscompilation, exactly as much as computing a wrong number, and
/// it is invisible to any test suite made only of well-behaved programs.
///
/// Div, Rem and ArrayLoad therefore count as side-effecting in
/// ir::mayHaveSideEffects, even though they produce values and look pure. The
/// corpus has programs specifically to catch a regression here
/// ("unused_division_must_still_trap", "unused_array_load_must_still_trap").
///
/// Note what this pass does NOT do: it never removes stores to local slots,
/// even ones that are obviously dead. Deciding that would need the kind of
/// memory analysis that mem2reg exists to avoid, and getting it wrong where
/// arrays alias would be a correctness bug rather than a missed win.
class DeadCodeElimination final : public Pass {
public:
  std::string_view name() const noexcept override { return "dce"; }

  std::string_view description() const noexcept override {
    return "remove instructions whose results are never used and that cannot trap";
  }

  PassStats run(Function& fn, const Module& /*module*/) override {
    PassStats stats;
    stats.passName = std::string(name());

    // Iterate to a fixed point: removing one instruction can make the
    // instruction that produced its operand dead in turn. A chain of five
    // copies needs five rounds to disappear entirely.
    for (;;) {
      std::vector<std::uint32_t> useCount(fn.nextValue, 0);

      for (const BasicBlock& block : fn.blocks) {
        for (const Instruction& inst : block.instructions) {
          inst.forEachUse([&](ValueId use) {
            if (use != kNoValue && use < useCount.size()) {
              ++useCount[use];
            }
          });
        }
      }

      std::uint32_t removedThisRound = 0;

      for (BasicBlock& block : fn.blocks) {
        auto& insts = block.instructions;
        std::vector<Instruction> kept;
        kept.reserve(insts.size());

        for (Instruction& inst : insts) {
          const bool producesValue = inst.dest != kNoValue;
          const bool resultUnused = producesValue && useCount[inst.dest] == 0;

          // The order of these conditions is the correctness-critical part:
          // side effects win over "the result is unused".
          if (resultUnused && !mayHaveSideEffects(inst.op)) {
            ++removedThisRound;
            continue;
          }

          // A call whose result is unused still has to run -- it may print or
          // write a global. Dropping just the destination register lets the
          // value it returns become dead, which can free up its operands.
          if (resultUnused && inst.op == Opcode::Call) {
            inst.dest = kNoValue;
            ++stats.instructionsRewritten;
          }

          kept.push_back(std::move(inst));
        }

        insts = std::move(kept);
      }

      stats.instructionsRemoved += removedThisRound;
      if (removedThisRound == 0) {
        break;
      }
    }

    computeCfg(fn);
    return stats;
  }
};

} // namespace

PassPtr createDeadCodeElimination() {
  return std::make_unique<DeadCodeElimination>();
}

} // namespace mloptc::opt
