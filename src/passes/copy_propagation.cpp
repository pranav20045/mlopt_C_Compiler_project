#include "mloptc/pass.hpp"

#include <unordered_map>

namespace mloptc::opt {

namespace {

using namespace ir;

/// Copy propagation: after `%b = move %a`, replace every use of %b with %a.
///
/// Why this is valid across the entire function, with no dataflow analysis:
/// every ValueId is assigned exactly once (the IR verifier enforces it). So if
/// %b is defined as a copy of %a, then %b equals %a everywhere %b is live --
/// there is no other assignment to either register that could make them differ
/// at some program points but not others.
///
/// In a conventional, non-SSA IR this pass would need reaching-definitions
/// analysis to answer "which definition of %b reaches this use?". Single
/// assignment makes that question trivial, which is most of why SSA-like forms
/// are worth the trouble.
///
/// The pass does not delete the `move` instructions themselves -- once nothing
/// uses %b, dead-code elimination removes them. Splitting the work that way
/// keeps each pass doing one thing, and it is a nice demonstration of why pass
/// ORDER matters: copy propagation followed by DCE removes the copies, while
/// DCE followed by copy propagation leaves them all in place.
class CopyPropagation final : public Pass {
public:
  std::string_view name() const noexcept override { return "copyprop"; }

  std::string_view description() const noexcept override {
    return "replace uses of a copied value with the original";
  }

  PassStats run(Function& fn, const Module& /*module*/) override {
    PassStats stats;
    stats.passName = std::string(name());

    // %dest -> the register it is a copy of.
    std::unordered_map<ValueId, ValueId> copyOf;

    for (const BasicBlock& block : fn.blocks) {
      for (const Instruction& inst : block.instructions) {
        if (inst.op == Opcode::Move && inst.dest != kNoValue && inst.a != kNoValue) {
          copyOf[inst.dest] = inst.a;
        }
      }
    }

    if (copyOf.empty()) {
      return stats;
    }

    // Collapse chains: %c = move %b, %b = move %a  =>  %c maps straight to %a.
    //
    // Resolving chains up front means a single rewrite pass is enough. The hop
    // limit is a guard, not an expectation: a cycle cannot occur under single
    // assignment, since %x = move %x would be a use before definition and the
    // verifier rejects it. Bounding the walk anyway means a bug elsewhere
    // produces a missed optimization rather than an infinite loop.
    const auto resolve = [&](ValueId id) {
      int hops = 0;
      while (hops++ < 1000) {
        const auto it = copyOf.find(id);
        if (it == copyOf.end()) {
          break;
        }
        id = it->second;
      }
      return id;
    };

    for (BasicBlock& block : fn.blocks) {
      for (Instruction& inst : block.instructions) {
        bool rewritten = false;
        inst.forEachUseMut([&](ValueId& use) {
          const ValueId root = resolve(use);
          if (root != use) {
            use = root;
            rewritten = true;
          }
        });
        if (rewritten) {
          ++stats.instructionsRewritten;
        }
      }
    }

    return stats;
  }
};

} // namespace

PassPtr createCopyPropagation() {
  return std::make_unique<CopyPropagation>();
}

} // namespace mloptc::opt
