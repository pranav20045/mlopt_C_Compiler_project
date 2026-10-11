#include "mloptc/pass.hpp"

#include <unordered_map>

namespace mloptc::opt {

namespace {

using namespace ir;

/// Redundant load elimination and store-to-load forwarding, within a block.
///
/// WHY THIS PASS EXISTS, and why it is the most important one here.
///
/// This IR is not in SSA form: local variables live in frame slots, and every
/// read of a variable is a LoadLocal producing a FRESH register. So the obvious
/// redundancy in
///
///     int p = x * y;
///     int q = x * y;
///
/// is invisible to common-subexpression elimination, because it sees
///
///     %0 = load.local 0   ; x        %3 = load.local 0   ; x again
///     %1 = load.local 1   ; y        %4 = load.local 1   ; y again
///     %2 = mul %0, %1                %5 = mul %3, %4
///
/// and `mul %0, %1` is simply not the same expression as `mul %3, %4`. The same
/// blindness defeats constant folding (a constant stored to a variable is
/// unknown when loaded back) and copy propagation.
///
/// Forwarding loads fixes all three at once: once the second load of slot 0 is
/// rewritten to `move %0`, copy propagation turns `mul %3, %4` into
/// `mul %0, %1`, and CSE finally recognises it.
///
/// This is what mem2reg does in a real compiler, in a block-local form that
/// needs no dominator tree and no phi nodes. The limitation is that nothing
/// crosses a block boundary -- a value stored before a loop is reloaded inside
/// it -- which is exactly the gap a real mem2reg closes.
///
/// CORRECTNESS -- what invalidates a remembered value:
///
///   * A store to the slot replaces it. Obvious.
///   * A CALL invalidates every GLOBAL, because the callee may assign to any
///     of them. This is the easy one to forget, and forgetting it means a
///     program that reads a global modified by a function it just called gets
///     the stale value.
///   * A call does NOT invalidate LOCAL slots. ToyC has no pointers, so a
///     callee cannot reach another function's frame. A local slot holding an
///     array handle is safe too: the callee may change the array's CONTENTS,
///     but the handle in our slot is unchanged.
///   * ArrayStore does not invalidate any slot, for the same reason: array
///     elements are not frame slots.
///   * Everything resets at a block boundary, since a block may be entered
///     from anywhere.
class LoadElimination final : public Pass {
public:
  std::string_view name() const noexcept override { return "loadelim"; }

  std::string_view description() const noexcept override {
    return "forward stored values to later loads and reuse repeated loads within a block";
  }

  PassStats run(Function& fn, const Module& /*module*/) override {
    PassStats stats;
    stats.passName = std::string(name());

    for (BasicBlock& block : fn.blocks) {
      // slot -> the register currently known to hold that slot's value.
      std::unordered_map<std::int32_t, ValueId> localValue;
      std::unordered_map<std::int32_t, ValueId> globalValue;

      for (Instruction& inst : block.instructions) {
        switch (inst.op) {
        case Opcode::StoreLocal:
          localValue[inst.imm] = inst.a;
          break;

        case Opcode::StoreGlobal:
          globalValue[inst.imm] = inst.a;
          break;

        case Opcode::LoadLocal: {
          const auto it = localValue.find(inst.imm);
          if (it != localValue.end()) {
            inst.op = Opcode::Move;
            inst.a = it->second;
            ++stats.instructionsRewritten;
          } else {
            // First load of this slot in the block: remember what register now
            // holds it, so a later load can reuse this one.
            localValue[inst.imm] = inst.dest;
          }
          break;
        }

        case Opcode::LoadGlobal: {
          const auto it = globalValue.find(inst.imm);
          if (it != globalValue.end()) {
            inst.op = Opcode::Move;
            inst.a = it->second;
            ++stats.instructionsRewritten;
          } else {
            globalValue[inst.imm] = inst.dest;
          }
          break;
        }

        case Opcode::Call:
          // The callee may assign to any global. Locals are unreachable to it.
          globalValue.clear();
          break;

        default:
          break;
        }
      }
    }

    return stats;
  }
};

} // namespace

PassPtr createLoadElimination() {
  return std::make_unique<LoadElimination>();
}

} // namespace mloptc::opt
