#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mloptc::json {

/// A minimal read-only JSON value.
///
/// Why hand-write this instead of taking a dependency: the project's rule is
/// "no heavy dependencies", and the only JSON we ever parse is a model file we
/// generate ourselves. That is a narrow enough job for ~150 lines. The parser
/// is strict -- it rejects anything it does not fully understand rather than
/// guessing -- because a silently misparsed model would show up as a model that
/// merely predicts badly, which is close to impossible to debug.
///
/// Not supported, deliberately: \u escapes, and numbers are all parsed as
/// double. Neither appears in our model format.
class Value {
public:
  enum class Kind { Null, Bool, Number, String, Array, Object };

  Value() = default;

  explicit Value(bool b) : kind_(Kind::Bool), bool_(b) {}

  explicit Value(double n) : kind_(Kind::Number), number_(n) {}

  explicit Value(std::string s) : kind_(Kind::String), string_(std::move(s)) {}

  [[nodiscard]] Kind kind() const noexcept { return kind_; }

  [[nodiscard]] bool isNull() const noexcept { return kind_ == Kind::Null; }

  [[nodiscard]] bool isNumber() const noexcept { return kind_ == Kind::Number; }

  [[nodiscard]] bool isArray() const noexcept { return kind_ == Kind::Array; }

  [[nodiscard]] bool isObject() const noexcept { return kind_ == Kind::Object; }

  [[nodiscard]] double asNumber(double fallback = 0.0) const noexcept {
    return kind_ == Kind::Number ? number_ : fallback;
  }

  [[nodiscard]] bool asBool(bool fallback = false) const noexcept {
    return kind_ == Kind::Bool ? bool_ : fallback;
  }

  [[nodiscard]] const std::string& asString() const noexcept { return string_; }

  [[nodiscard]] const std::vector<Value>& items() const noexcept { return array_; }

  /// Member lookup. Returns nullptr if absent, so callers must handle a
  /// malformed model explicitly rather than silently reading a default.
  [[nodiscard]] const Value* find(std::string_view key) const;

  [[nodiscard]] std::size_t size() const noexcept {
    return kind_ == Kind::Array ? array_.size() : object_.size();
  }

  // Builders, used by the parser.
  static Value makeArray(std::vector<Value> items);
  static Value makeObject(std::map<std::string, Value, std::less<>> members);

private:
  Kind kind_ = Kind::Null;
  bool bool_ = false;
  double number_ = 0.0;
  std::string string_;
  std::vector<Value> array_;
  std::map<std::string, Value, std::less<>> object_;
};

/// Parses `text`. On failure returns nullopt and sets `error`.
[[nodiscard]] std::optional<Value> parse(std::string_view text, std::string& error);

} // namespace mloptc::json
