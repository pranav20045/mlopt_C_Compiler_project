#include "mloptc/ml_scheduler.hpp"

#include "mloptc/json.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <sstream>

namespace mloptc::ml {

namespace {

bool readNumberArray(const json::Value& parent, std::string_view key, std::vector<double>& out,
                     std::string& error) {
  const json::Value* array = parent.find(key);
  if (array == nullptr || !array->isArray()) {
    error = "model: missing or non-array field '" + std::string(key) + "'";
    return false;
  }
  out.clear();
  out.reserve(array->size());
  for (const json::Value& item : array->items()) {
    if (!item.isNumber()) {
      error = "model: non-numeric entry in '" + std::string(key) + "'";
      return false;
    }
    out.push_back(item.asNumber());
  }
  return true;
}

bool readIntArray(const json::Value& parent, std::string_view key, std::vector<int>& out,
                  std::string& error) {
  std::vector<double> numbers;
  if (!readNumberArray(parent, key, numbers, error)) {
    return false;
  }
  out.clear();
  out.reserve(numbers.size());
  for (const double n : numbers) {
    out.push_back(static_cast<int>(n));
  }
  return true;
}

bool readStringArray(const json::Value& parent, std::string_view key,
                     std::vector<std::string>& out, std::string& error) {
  const json::Value* array = parent.find(key);
  if (array == nullptr || !array->isArray()) {
    error = "model: missing or non-array field '" + std::string(key) + "'";
    return false;
  }
  out.clear();
  for (const json::Value& item : array->items()) {
    if (item.kind() != json::Value::Kind::String) {
      error = "model: non-string entry in '" + std::string(key) + "'";
      return false;
    }
    out.push_back(item.asString());
  }
  return true;
}

} // namespace

double Model::Tree::evaluate(const std::vector<double>& x) const {
  int node = 0;
  // Bounded, so a malformed tree (a cycle, say) cannot hang the compiler.
  for (int steps = 0; steps < 1000; ++steps) {
    if (node < 0 || static_cast<std::size_t>(node) >= feature.size()) {
      return 0.0;
    }
    if (left[static_cast<std::size_t>(node)] < 0) {
      return value[static_cast<std::size_t>(node)]; // leaf
    }

    const int featureIndex = feature[static_cast<std::size_t>(node)];
    const double featureValue =
        featureIndex >= 0 && static_cast<std::size_t>(featureIndex) < x.size()
            ? x[static_cast<std::size_t>(featureIndex)]
            : 0.0;

    // scikit-learn's convention: go left when value <= threshold.
    node = featureValue <= threshold[static_cast<std::size_t>(node)]
               ? left[static_cast<std::size_t>(node)]
               : right[static_cast<std::size_t>(node)];
  }
  return 0.0;
}

std::optional<Model> Model::loadFromJson(std::string_view text, std::string& error) {
  auto parsed = json::parse(text, error);
  if (!parsed) {
    return std::nullopt;
  }
  const json::Value& root = *parsed;

  Model model;

  // --- feature layout check -------------------------------------------------
  // A model trained against a different feature list would still load and still
  // predict -- just from the wrong numbers, producing a scheduler that quietly
  // picks badly. Refusing to load is far better than debugging that later.
  std::vector<std::string> trainedFeatureNames;
  if (!readStringArray(root, "feature_names", trainedFeatureNames, error)) {
    return std::nullopt;
  }

  const auto& names = featureNames();
  if (trainedFeatureNames.size() < kFeatureCount) {
    error = "model: expects " + std::to_string(trainedFeatureNames.size()) +
            " program features, this build has " + std::to_string(kFeatureCount);
    return std::nullopt;
  }
  for (std::size_t i = 0; i < kFeatureCount; ++i) {
    if (trainedFeatureNames[i] != names[i]) {
      error = "model: feature " + std::to_string(i) + " is '" + trainedFeatureNames[i] +
              "', this build has '" + std::string(names[i]) +
              "'. Retrain against the current compiler.";
      return std::nullopt;
    }
  }

  if (!readStringArray(root, "pass_vocabulary", model.passVocabulary_, error)) {
    return std::nullopt;
  }

  const json::Value* maxLen = root.find("max_sequence_length");
  if (maxLen == nullptr || !maxLen->isNumber()) {
    error = "model: missing 'max_sequence_length'";
    return std::nullopt;
  }
  model.maxSequenceLength_ = static_cast<std::size_t>(maxLen->asNumber());

  if (const json::Value* base = root.find("base_score"); base != nullptr && base->isNumber()) {
    model.baseScore_ = base->asNumber();
  }
  if (const json::Value* lr = root.find("learning_rate"); lr != nullptr && lr->isNumber()) {
    model.learningRate_ = lr->asNumber();
  }

  // --- candidate sequences --------------------------------------------------
  const json::Value* candidates = root.find("candidates");
  if (candidates == nullptr || !candidates->isArray() || candidates->size() == 0) {
    error = "model: missing or empty 'candidates'";
    return std::nullopt;
  }
  for (const json::Value& candidate : candidates->items()) {
    if (!candidate.isArray()) {
      error = "model: each candidate must be an array of pass names";
      return std::nullopt;
    }
    std::vector<std::string> sequence;
    for (const json::Value& pass : candidate.items()) {
      if (pass.kind() != json::Value::Kind::String) {
        error = "model: candidate contains a non-string pass name";
        return std::nullopt;
      }
      sequence.push_back(pass.asString());
    }
    model.candidates_.push_back(std::move(sequence));
  }

  // --- trees ----------------------------------------------------------------
  const json::Value* trees = root.find("trees");
  if (trees == nullptr || !trees->isArray()) {
    error = "model: missing 'trees'";
    return std::nullopt;
  }

  for (const json::Value& treeJson : trees->items()) {
    Tree tree;
    if (!readIntArray(treeJson, "feature", tree.feature, error) ||
        !readNumberArray(treeJson, "threshold", tree.threshold, error) ||
        !readIntArray(treeJson, "left", tree.left, error) ||
        !readIntArray(treeJson, "right", tree.right, error) ||
        !readNumberArray(treeJson, "value", tree.value, error)) {
      return std::nullopt;
    }

    const std::size_t n = tree.feature.size();
    if (tree.threshold.size() != n || tree.left.size() != n || tree.right.size() != n ||
        tree.value.size() != n) {
      error = "model: tree arrays have inconsistent lengths";
      return std::nullopt;
    }
    model.trees_.push_back(std::move(tree));
  }

  if (model.trees_.empty()) {
    error = "model: contains no trees";
    return std::nullopt;
  }

  return model;
}

std::optional<Model> Model::loadFromFile(const std::string& path, std::string& error) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    error = "cannot open model file '" + path + "'";
    return std::nullopt;
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return loadFromJson(buffer.str(), error);
}

std::vector<double> Model::buildInput(const FeatureVector& programFeatures,
                                      const std::vector<std::string>& sequence) const {
  // LAYOUT -- must match ml/train.py exactly:
  //   [0 .. kFeatureCount)                  the program's IR features
  //   then, per pass in the vocabulary:     how many times it appears
  //   then, per position up to maxLength:   index of the pass at that position,
  //                                         or -1 if the sequence is shorter
  //
  // The counts tell the model WHICH passes run; the positions tell it in what
  // ORDER. Counts alone would make every permutation look identical, which is
  // precisely the thing being predicted.
  std::vector<double> x;
  x.reserve(kFeatureCount + passVocabulary_.size() + maxSequenceLength_);

  for (const double value : programFeatures) {
    x.push_back(value);
  }

  for (const std::string& pass : passVocabulary_) {
    x.push_back(static_cast<double>(
        std::count(sequence.begin(), sequence.end(), pass)));
  }

  for (std::size_t i = 0; i < maxSequenceLength_; ++i) {
    if (i >= sequence.size()) {
      x.push_back(-1.0);
      continue;
    }
    const auto it = std::find(passVocabulary_.begin(), passVocabulary_.end(), sequence[i]);
    x.push_back(it == passVocabulary_.end()
                    ? -1.0
                    : static_cast<double>(std::distance(passVocabulary_.begin(), it)));
  }

  return x;
}

double Model::predict(const FeatureVector& programFeatures,
                      const std::vector<std::string>& passSequence) const {
  const std::vector<double> x = buildInput(programFeatures, passSequence);

  // Gradient boosting: start from the base prediction and add each tree's
  // contribution, scaled by the learning rate.
  double sum = baseScore_;
  for (const Tree& tree : trees_) {
    sum += learningRate_ * tree.evaluate(x);
  }
  return sum;
}

std::vector<std::string> Model::choosePipeline(const ir::Module& module) const {
  const FeatureVector features = extractFeatures(module);

  // Lower predicted cost is better. Ties go to the earlier candidate, which
  // makes the choice deterministic -- the compiler must produce the same output
  // for the same input every time, model or no model.
  const std::vector<std::string>* best = nullptr;
  double bestScore = std::numeric_limits<double>::infinity();

  for (const std::vector<std::string>& candidate : candidates_) {
    const double score = predict(features, candidate);
    if (score < bestScore) {
      bestScore = score;
      best = &candidate;
    }
  }

  return best != nullptr ? *best : std::vector<std::string>{};
}

} // namespace mloptc::ml
