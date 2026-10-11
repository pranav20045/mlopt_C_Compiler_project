#pragma once

#include "mloptc/ir.hpp"

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace mloptc::ml {

/// The feature vector describing a program's IR.
///
/// These are the numbers the pass-ordering model sees. The design constraints
/// are worth stating, because they rule out a lot of tempting ideas:
///
///  1. **Static only.** Features are read off the IR without running anything.
///     A feature that needed a trial execution would make the compiler's own
///     -O-ml path as slow as the thing it is trying to optimize.
///
///  2. **Scale-free where possible.** Raw counts tell the model mostly how BIG
///     a program is, which it can learn to key on without learning anything
///     about structure. Ratios (what fraction of instructions are memory
///     operations) generalise from the training set to programs of a different
///     size; raw counts do not. Both are included, with sizes log-scaled.
///
///  3. **Cheap.** Every feature below is computable in one or two linear walks
///     over the IR.
enum class Feature : std::uint8_t {
  LogInstructionCount,
  LogBlockCount,
  LogFunctionCount,

  AvgBlockSize,
  MaxBlockSize,

  /// Back edges per block: our proxy for loopiness. A back edge is an edge to
  /// a block with a lower id, which for a CFG built from structured control
  /// flow means a loop. Loops are the single biggest driver of dynamic cost,
  /// so this is the most informative structural feature here.
  BackEdgeRatio,
  MaxLoopDepthEstimate,

  /// Fraction of blocks ending in a conditional branch.
  BranchDensity,
  AvgSuccessors,

  // --- instruction mix, each as a fraction of all instructions ---
  ArithmeticRatio,
  ComparisonRatio,
  MemoryRatio, ///< loads and stores on scalars
  ArrayRatio,  ///< array loads and stores
  CallRatio,
  ConstRatio,
  MoveRatio,
  DivRemRatio, ///< trapping arithmetic, which blocks several optimizations
  PrintRatio,

  /// How much redundancy is visible: the fraction of pure instructions whose
  /// (opcode, operands) triple appears more than once in the same block. A
  /// direct hint at how much CSE has to work with.
  RedundancyRatio,

  /// Fraction of loads whose slot is stored to earlier in the same block --
  /// what loadelim can forward.
  ForwardableLoadRatio,

  /// Fraction of instructions whose result is never used. What DCE can remove,
  /// before accounting for side effects.
  DeadInstructionRatio,

  /// Fraction of binary operations with at least one constant operand. What
  /// constant folding has to work with.
  ConstOperandRatio,

  Count_ ///< not a feature; the number of features
};

inline constexpr std::size_t kFeatureCount = static_cast<std::size_t>(Feature::Count_);

using FeatureVector = std::array<double, kFeatureCount>;

/// Stable names, used in the exported model JSON and in the Python tooling.
/// The ORDER must never change without retraining, since a model stores
/// feature indices, not names.
[[nodiscard]] const std::array<std::string_view, kFeatureCount>& featureNames() noexcept;

[[nodiscard]] FeatureVector extractFeatures(const ir::Module& module);

/// Renders a feature vector as a single line of JSON.
[[nodiscard]] std::string featuresToJson(const FeatureVector& features);

} // namespace mloptc::ml
