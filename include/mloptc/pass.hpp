#pragma once

#include "mloptc/ir.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace mloptc::opt {

/// What a pass changed. Reported per-pass so the CLI can show where the wins
/// came from and so the ML dataset generator has something to record besides
/// the final instruction count.
struct PassStats {
  std::string passName;
  std::uint32_t instructionsRemoved = 0;
  std::uint32_t instructionsRewritten = 0;
  std::uint32_t blocksRemoved = 0;

  [[nodiscard]] bool changed() const noexcept {
    return instructionsRemoved > 0 || instructionsRewritten > 0 || blocksRemoved > 0;
  }
};

/// Base class for an optimization pass.
///
/// A pass transforms one function at a time and reports what it did. Keeping
/// passes function-local (rather than whole-module) is what makes them
/// independently orderable, which is the entire premise of the ML scheduler --
/// if passes had module-wide state, their order would interact in ways a small
/// model could not learn.
class Pass {
public:
  Pass() = default;
  Pass(const Pass&) = delete;
  Pass& operator=(const Pass&) = delete;
  virtual ~Pass() = default;

  /// Short CLI name, e.g. "constfold".
  [[nodiscard]] virtual std::string_view name() const noexcept = 0;

  /// One-line description for --help.
  [[nodiscard]] virtual std::string_view description() const noexcept = 0;

  /// Transforms `fn` in place and returns what changed.
  ///
  /// A pass MUST leave the IR valid (ir::verify) and MUST preserve observable
  /// behaviour. In debug builds the pass manager checks the first after every
  /// run; the differential tester checks the second.
  [[nodiscard]] virtual PassStats run(ir::Function& fn, const ir::Module& module) = 0;
};

using PassPtr = std::unique_ptr<Pass>;

// --- the passes ---------------------------------------------------------------

[[nodiscard]] PassPtr createConstantFolding();
[[nodiscard]] PassPtr createCopyPropagation();
[[nodiscard]] PassPtr createDeadCodeElimination();
[[nodiscard]] PassPtr createCommonSubexpressionElimination();
[[nodiscard]] PassPtr createLoadElimination();

/// Every pass, in a fixed order, one instance each. Used by the CLI to list
/// what is available and by the ML tooling to enumerate the search space.
[[nodiscard]] std::vector<PassPtr> createAllPasses();

/// The names of all passes, in the same order as createAllPasses().
[[nodiscard]] std::vector<std::string> allPassNames();

/// Constructs a single pass by its CLI name. Returns nullptr if unknown.
[[nodiscard]] PassPtr createPassByName(std::string_view name);

} // namespace mloptc::opt
