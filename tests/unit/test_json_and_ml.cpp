#include "mloptc/driver.hpp"
#include "mloptc/ir_features.hpp"
#include "mloptc/json.hpp"
#include "mloptc/lowering.hpp"
#include "mloptc/ml_scheduler.hpp"
#include "mloptc/pass.hpp"

#include <gtest/gtest.h>
#include <string>

using namespace mloptc;

namespace {

json::Value parseOk(std::string_view text) {
  std::string error;
  auto value = json::parse(text, error);
  EXPECT_TRUE(value.has_value()) << "parse failed: " << error;
  return value ? std::move(*value) : json::Value{};
}

bool parseFails(std::string_view text) {
  std::string error;
  return !json::parse(text, error).has_value();
}

ir::Module moduleFor(std::string_view source) {
  DiagnosticEngine diags;
  auto program = compileFrontend(source, diags);
  EXPECT_TRUE(program.has_value());
  return program ? lower(*program) : ir::Module{};
}

} // namespace

// =============================================================================
// JSON parser
// =============================================================================

TEST(Json, ParsesScalars) {
  EXPECT_EQ(parseOk("42").asNumber(), 42.0);
  EXPECT_EQ(parseOk("-1.5e2").asNumber(), -150.0);
  EXPECT_TRUE(parseOk("true").asBool());
  EXPECT_FALSE(parseOk("false").asBool(true));
  EXPECT_TRUE(parseOk("null").isNull());
  EXPECT_EQ(parseOk("\"hello\"").asString(), "hello");
}

TEST(Json, ParsesEscapes) {
  EXPECT_EQ(parseOk(R"("a\nb\tc\"d\\e")").asString(), "a\nb\tc\"d\\e");
}

TEST(Json, ParsesArrays) {
  const json::Value array = parseOk("[1, 2, 3]");
  ASSERT_TRUE(array.isArray());
  ASSERT_EQ(array.size(), 3U);
  EXPECT_EQ(array.items()[2].asNumber(), 3.0);
}

TEST(Json, ParsesNestedObjects) {
  const json::Value root = parseOk(R"({"a": {"b": [1, {"c": 2}]}})");
  const json::Value* a = root.find("a");
  ASSERT_NE(a, nullptr);
  const json::Value* b = a->find("b");
  ASSERT_NE(b, nullptr);
  ASSERT_EQ(b->size(), 2U);
  EXPECT_EQ(b->items()[1].find("c")->asNumber(), 2.0);
}

TEST(Json, ParsesEmptyContainers) {
  EXPECT_TRUE(parseOk("[]").isArray());
  EXPECT_TRUE(parseOk("{}").isObject());
  EXPECT_EQ(parseOk("[]").size(), 0U);
}

TEST(Json, FindReturnsNullForMissingKey) {
  // The API returns a pointer rather than a default, so a malformed model file
  // has to be handled explicitly instead of silently reading zeros.
  EXPECT_EQ(parseOk(R"({"a": 1})").find("b"), nullptr);
}

TEST(Json, RejectsMalformedInput) {
  // The parser is strict on purpose: a silently misparsed model would show up
  // as a model that merely predicts badly, which is nearly impossible to debug.
  EXPECT_TRUE(parseFails(""));
  EXPECT_TRUE(parseFails("{"));
  EXPECT_TRUE(parseFails("[1, 2"));
  EXPECT_TRUE(parseFails("{\"a\" 1}"));
  EXPECT_TRUE(parseFails("{a: 1}"));
  EXPECT_TRUE(parseFails("[1, 2] extra"));
  EXPECT_TRUE(parseFails("\"unterminated"));
  EXPECT_TRUE(parseFails("tru"));
  // A \u escape. Assembled from single characters so that nothing between here
  // and the parser -- editor, shell, or build script -- can interpret it first.
  // The parser rejects \u rather than guessing at the code point; see json.hpp.
  std::string unicodeEscape;
  unicodeEscape += '"';
  unicodeEscape += '\\';
  unicodeEscape += "u0041";
  unicodeEscape += '"';
  EXPECT_TRUE(parseFails(unicodeEscape)) << "unsupported escapes must be rejected";
}

TEST(Json, RejectsDeeplyNestedInput) {
  // A model file is untrusted input as far as this parser is concerned, and a
  // recursive-descent parser would otherwise overflow the stack.
  const std::string deep = std::string(5000, '[') + std::string(5000, ']');
  EXPECT_TRUE(parseFails(deep));
}

// =============================================================================
// IR features
// =============================================================================

TEST(Features, ProducesOneValuePerName) {
  const ir::Module module = moduleFor("int main() { print(1); return 0; }");
  const ml::FeatureVector features = ml::extractFeatures(module);
  EXPECT_EQ(features.size(), ml::featureNames().size());
}

TEST(Features, AreFiniteForTrivialPrograms) {
  // A NaN would silently poison any model trained on it: most learners either
  // crash or quietly treat it as a split point.
  const ir::Module module = moduleFor("int main() { return 0; }");
  const ml::FeatureVector features = ml::extractFeatures(module);
  for (std::size_t i = 0; i < features.size(); ++i) {
    EXPECT_EQ(features[i], features[i]) << ml::featureNames()[i] << " is NaN";
  }
}

TEST(Features, LoopsRaiseTheBackEdgeRatio) {
  const ir::Module straight = moduleFor("int main() { print(1); print(2); return 0; }");
  const ir::Module looping =
      moduleFor("int main() { for (int i = 0; i < 9; i = i + 1) { print(i); } return 0; }");

  const auto index = static_cast<std::size_t>(ml::Feature::BackEdgeRatio);
  EXPECT_GT(ml::extractFeatures(looping)[index], ml::extractFeatures(straight)[index]);
}

TEST(Features, NestedLoopsRaiseTheDepthEstimate) {
  const ir::Module shallow =
      moduleFor("int main() { for (int i = 0; i < 3; i = i + 1) { print(i); } return 0; }");
  const ir::Module nested = moduleFor(R"(
    int main() {
      for (int i = 0; i < 3; i = i + 1) {
        for (int j = 0; j < 3; j = j + 1) { print(i * j); }
      }
      return 0;
    }
  )");

  const auto index = static_cast<std::size_t>(ml::Feature::MaxLoopDepthEstimate);
  EXPECT_GT(ml::extractFeatures(nested)[index], ml::extractFeatures(shallow)[index]);
}

TEST(Features, RedundancyIsDetected) {
  const ir::Module module = moduleFor(R"(
    int f(int x, int y) { return (x * y) + (x * y) + (x * y); }
    int main() { print(f(2, 3)); return 0; }
  )");
  // The raw IR loads x and y separately each time, so register-level redundancy
  // is low before loadelim -- the feature measures what CSE could take AS-IS,
  // which is exactly what the model needs to know.
  const auto index = static_cast<std::size_t>(ml::Feature::ForwardableLoadRatio);
  EXPECT_GT(ml::extractFeatures(module)[index], 0.0);
}

TEST(Features, AreDeterministic) {
  const ir::Module module = moduleFor("int main() { int a[4]; a[0] = 1; print(a[0]); return 0; }");
  EXPECT_EQ(ml::extractFeatures(module), ml::extractFeatures(module));
}

// =============================================================================
// Model loading
// =============================================================================

namespace {

/// A tiny but structurally valid model: one tree, one leaf.
///
/// Built from parts rather than edited with string surgery, so a test that
/// wants different candidates says so directly instead of hoping a find() lands
/// in the right place.
std::string makeModelJson(
    const std::string& featureNamesJson,
    const std::string& candidatesJson = R"([["loadelim", "dce"], ["loadelim", "copyprop", "dce"]])",
    const std::string& treesJson = R"([{"feature": [-2], "threshold": [-2.0], "left": [-1],)"
                                   R"( "right": [-1], "value": [0.0]}])") {
  return R"({
    "format_version": 1,
    "feature_names": )" +
         featureNamesJson + R"(,
    "pass_vocabulary": ["loadelim", "constfold", "copyprop", "cse", "dce"],
    "max_sequence_length": 12,
    "base_score": -0.1,
    "learning_rate": 0.1,
    "selection_tolerance": 0.01,
    "candidates": )" +
         candidatesJson + R"(,
    "trees": )" +
         treesJson + R"(
  })";
}

std::string currentFeatureNamesJson() {
  std::string out = "[";
  const auto& names = ml::featureNames();
  for (std::size_t i = 0; i < names.size(); ++i) {
    if (i > 0) {
      out += ",";
    }
    out += "\"" + std::string(names[i]) + "\"";
  }
  for (const char* extra :
       {"count_loadelim", "count_constfold", "count_copyprop", "count_cse", "count_dce"}) {
    out += std::string(",\"") + extra + "\"";
  }
  out += "]";
  return out;
}

} // namespace

TEST(MlModel, LoadsAWellFormedModel) {
  std::string error;
  const auto model = ml::Model::loadFromJson(makeModelJson(currentFeatureNamesJson()), error);
  EXPECT_TRUE(model.has_value()) << error;
}

TEST(MlModel, RejectsAFeatureListThatDoesNotMatchTheBuild) {
  // THE check that matters. A model trained against different features would
  // still load and still predict -- from the wrong numbers -- producing a
  // scheduler that quietly picks badly. Refusing is far better than debugging
  // that months later.
  std::string error;
  const auto model =
      ml::Model::loadFromJson(makeModelJson(R"(["not_a_real_feature","another"])"), error);

  EXPECT_FALSE(model.has_value());
  EXPECT_NE(error.find("feature"), std::string::npos) << error;
}

TEST(MlModel, RejectsAModelWithNoTrees) {
  std::string error;
  EXPECT_FALSE(ml::Model::loadFromJson(
                   makeModelJson(currentFeatureNamesJson(), R"([["loadelim"]])", "[]"), error)
                   .has_value());
}

TEST(MlModel, RejectsMalformedJson) {
  std::string error;
  EXPECT_FALSE(ml::Model::loadFromJson("{ not json", error).has_value());
}

TEST(MlModel, NeverSelectsAnEmptyPipeline) {
  // A bug found by benchmark comparison: the empty sequence is the shortest
  // candidate, so once "prefer the shortest among indistinguishable
  // candidates" was added, it started winning -- turning -O-ml into -O0 on
  // exactly the programs the model understood least.
  std::string error;
  const auto model = ml::Model::loadFromJson(
      makeModelJson(currentFeatureNamesJson(), R"([[], ["loadelim", "dce"]])"), error);
  ASSERT_TRUE(model.has_value()) << error;

  const ir::Module module = moduleFor("int main() { int x = 1 + 2; print(x); return 0; }");
  EXPECT_FALSE(model->choosePipeline(module).empty()) << "-O-ml selected 'run no passes at all'";
}

TEST(MlModel, NeverSelectsEmptyEvenWhenItScoresBest) {
  // The regression test for the SECOND form of the empty-pipeline bug.
  //
  // The first fix skipped the empty sequence in the selection loop but still
  // let it set the minimum score. If the empty sequence was the strict
  // minimum, no surviving candidate could satisfy
  // `score <= bestScore + tolerance`, and the function returned {} -- the
  // exact outcome the guard was written to prevent.
  //
  // NeverSelectsAnEmptyPipeline cannot catch this: its single-leaf tree gives
  // every candidate an identical score, so the minimum is always attainable.
  // This model has a tree that splits on a pass-count feature, making the
  // empty sequence (which runs no passes) score strictly lower than any other.
  //
  // Feature index kFeatureCount is "count_loadelim": 0 for the empty
  // sequence, >= 1 for any candidate containing loadelim.
  const std::string treeWithSplit =
      R"([{"feature": [)" + std::to_string(ml::kFeatureCount) +
      R"(, -2, -2], "threshold": [0.5, -2.0, -2.0],)"
      R"( "left": [1, -1, -1], "right": [2, -1, -1], "value": [0.0, -99.0, 0.0]}])";

  std::string error;
  const auto model = ml::Model::loadFromJson(
      makeModelJson(currentFeatureNamesJson(),
                    R"([[], ["loadelim", "dce"], ["loadelim", "copyprop", "dce"]])", treeWithSplit),
      error);
  ASSERT_TRUE(model.has_value()) << error;

  const ir::Module module = moduleFor("int main() { int x = 1 + 2; print(x); return 0; }");
  const std::vector<std::string> chosen = model->choosePipeline(module);

  EXPECT_FALSE(chosen.empty()) << "-O-ml returned an empty pipeline because the excluded empty "
                                  "candidate set the minimum score";
}

TEST(MlModel, SelectionIsDeterministic) {
  // The compiler must produce the same output for the same input every time,
  // model or no model.
  std::string error;
  const auto model = ml::Model::loadFromJson(makeModelJson(currentFeatureNamesJson()), error);
  ASSERT_TRUE(model.has_value()) << error;

  const ir::Module module =
      moduleFor("int main() { for (int i=0;i<4;i=i+1) { print(i); } return 0; }");
  EXPECT_EQ(model->choosePipeline(module), model->choosePipeline(module));
}

TEST(MlModel, SelectsOnlyKnownPasses) {
  std::string error;
  const auto model = ml::Model::loadFromJson(makeModelJson(currentFeatureNamesJson()), error);
  ASSERT_TRUE(model.has_value()) << error;

  const ir::Module module = moduleFor("int main() { print(1); return 0; }");
  for (const std::string& name : model->choosePipeline(module)) {
    EXPECT_NE(opt::createPassByName(name), nullptr) << "selected unknown pass '" << name << "'";
  }
}
