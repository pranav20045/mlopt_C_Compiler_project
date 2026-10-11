#pragma once

#include "mloptc/ir.hpp"
#include "mloptc/ir_features.hpp"

#include <optional>
#include <string>
#include <vector>

namespace mloptc::ml {

/// A gradient-boosted regression forest, loaded from the JSON that
/// ml/train.py exports.
///
/// Inference is a few hundred comparisons, so it runs inside the compiler with
/// no dependency on Python, scikit-learn, or anything else at compile time.
/// That matters for the claim being made: `-O-ml` has to be a real compiler
/// flag, not a Python script that drives the compiler from outside.
///
/// What the model predicts: for one (program, pass-sequence) pair, the expected
/// dynamic instruction count RELATIVE to no optimization, in log space. Lower
/// is better. Predicting a ratio rather than an absolute count is what lets a
/// model trained on small programs say anything useful about large ones -- an
/// absolute count would mostly encode program size.
class Model {
public:
  /// Loads from JSON text. Returns nullopt and fills `error` on any problem,
  /// including a feature list that does not match this build -- a mismatch
  /// there would silently feed the model the wrong numbers.
  [[nodiscard]] static std::optional<Model> loadFromJson(std::string_view text, std::string& error);

  [[nodiscard]] static std::optional<Model> loadFromFile(const std::string& path,
                                                         std::string& error);

  /// Predicted log-ratio for one candidate sequence. Lower is better.
  [[nodiscard]] double predict(const FeatureVector& programFeatures,
                               const std::vector<std::string>& passSequence) const;

  /// The candidate sequences the model was trained to choose between.
  [[nodiscard]] const std::vector<std::vector<std::string>>& candidates() const noexcept {
    return candidates_;
  }

  /// Picks a pipeline for this module.
  ///
  /// Not simply argmin. Measurement showed that on this compiler many
  /// different sequences reach the SAME optimized code -- no candidate beat the
  /// fixed -O2 pipeline on any of 160 measured programs -- while differing
  /// enormously in how many passes they run. So the rule is: take the best
  /// predicted cost, then among every candidate whose prediction is within
  /// `tolerance` of it (i.e. indistinguishable given the model's own error),
  /// choose the SHORTEST. The model picks the quality; the tie-break buys
  /// compile time back for free.
  [[nodiscard]] std::vector<std::string> choosePipeline(const ir::Module& module) const;

  /// Predictions closer together than this are treated as equal. Exported by
  /// ml/train.py as the model's held-out mean absolute error: differences
  /// smaller than the model's own error are noise, not signal.
  [[nodiscard]] double tolerance() const noexcept { return tolerance_; }

private:
  /// One regression tree, stored as parallel arrays exactly as scikit-learn
  /// represents it. A node is a leaf when its children are -1.
  struct Tree {
    std::vector<int> feature;
    std::vector<double> threshold;
    std::vector<int> left;
    std::vector<int> right;
    std::vector<double> value;

    [[nodiscard]] double evaluate(const std::vector<double>& x) const;
  };

  /// Builds the model's input: program features followed by the encoding of a
  /// pass sequence. Both halves must be laid out exactly as ml/train.py does
  /// it, which is why that layout is described in one place and asserted on
  /// load.
  [[nodiscard]] std::vector<double> buildInput(const FeatureVector& programFeatures,
                                               const std::vector<std::string>& sequence) const;

  std::vector<Tree> trees_;
  double baseScore_ = 0.0;
  double learningRate_ = 1.0;

  /// Pass names in the order train.py encoded them.
  std::vector<std::string> passVocabulary_;

  /// Longest sequence the encoding has room for.
  std::size_t maxSequenceLength_ = 0;

  std::vector<std::vector<std::string>> candidates_;

  double tolerance_ = 0.0;
};

} // namespace mloptc::ml
