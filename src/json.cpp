#include "mloptc/json.hpp"

#include <cctype>
#include <charconv>
#include <cstdlib>

namespace mloptc::json {

const Value* Value::find(std::string_view key) const {
  if (kind_ != Kind::Object) {
    return nullptr;
  }
  const auto it = object_.find(key);
  return it == object_.end() ? nullptr : &it->second;
}

Value Value::makeArray(std::vector<Value> items) {
  Value v;
  v.kind_ = Kind::Array;
  v.array_ = std::move(items);
  return v;
}

Value Value::makeObject(std::map<std::string, Value, std::less<>> members) {
  Value v;
  v.kind_ = Kind::Object;
  v.object_ = std::move(members);
  return v;
}

namespace {

class Parser {
public:
  Parser(std::string_view text, std::string& error) : text_(text), error_(error) {}

  std::optional<Value> parseDocument() {
    skipWhitespace();
    auto value = parseValue(0);
    if (!value) {
      return std::nullopt;
    }
    skipWhitespace();
    if (pos_ != text_.size()) {
      return fail("trailing characters after the top-level value");
    }
    return value;
  }

private:
  std::optional<Value> fail(const std::string& message) {
    if (error_.empty()) {
      error_ = message + " at offset " + std::to_string(pos_);
    }
    return std::nullopt;
  }

  [[nodiscard]] bool atEnd() const noexcept { return pos_ >= text_.size(); }

  [[nodiscard]] char peek() const noexcept { return atEnd() ? '\0' : text_[pos_]; }

  void skipWhitespace() {
    while (!atEnd()) {
      const char c = text_[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  bool consume(char expected) {
    if (peek() == expected) {
      ++pos_;
      return true;
    }
    return false;
  }

  bool consumeLiteral(std::string_view literal) {
    if (text_.compare(pos_, literal.size(), literal) == 0) {
      pos_ += literal.size();
      return true;
    }
    return false;
  }

  /// Depth is bounded for the same reason the ToyC parser bounds it: a
  /// recursive-descent parser on deeply nested input would otherwise overflow
  /// the stack, and a model file is untrusted input as far as this code is
  /// concerned.
  std::optional<Value> parseValue(int depth) {
    if (depth > 100) {
      return fail("JSON nested too deeply");
    }
    skipWhitespace();

    if (atEnd()) {
      return fail("unexpected end of input");
    }

    switch (peek()) {
    case '{':
      return parseObject(depth);
    case '[':
      return parseArray(depth);
    case '"': {
      auto s = parseString();
      if (!s) {
        return std::nullopt;
      }
      return Value(std::move(*s));
    }
    case 't':
      return consumeLiteral("true") ? std::optional<Value>(Value(true)) : fail("expected true");
    case 'f':
      return consumeLiteral("false") ? std::optional<Value>(Value(false)) : fail("expected false");
    case 'n':
      return consumeLiteral("null") ? std::optional<Value>(Value()) : fail("expected null");
    default:
      return parseNumber();
    }
  }

  std::optional<Value> parseObject(int depth) {
    ++pos_; // '{'
    std::map<std::string, Value, std::less<>> members;

    skipWhitespace();
    if (consume('}')) {
      return Value::makeObject(std::move(members));
    }

    for (;;) {
      skipWhitespace();
      if (peek() != '"') {
        return fail("expected a key string in object");
      }
      auto key = parseString();
      if (!key) {
        return std::nullopt;
      }

      skipWhitespace();
      if (!consume(':')) {
        return fail("expected ':' after object key");
      }

      auto value = parseValue(depth + 1);
      if (!value) {
        return std::nullopt;
      }
      members.emplace(std::move(*key), std::move(*value));

      skipWhitespace();
      if (consume(',')) {
        continue;
      }
      if (consume('}')) {
        return Value::makeObject(std::move(members));
      }
      return fail("expected ',' or '}' in object");
    }
  }

  std::optional<Value> parseArray(int depth) {
    ++pos_; // '['
    std::vector<Value> items;

    skipWhitespace();
    if (consume(']')) {
      return Value::makeArray(std::move(items));
    }

    for (;;) {
      auto value = parseValue(depth + 1);
      if (!value) {
        return std::nullopt;
      }
      items.push_back(std::move(*value));

      skipWhitespace();
      if (consume(',')) {
        continue;
      }
      if (consume(']')) {
        return Value::makeArray(std::move(items));
      }
      return fail("expected ',' or ']' in array");
    }
  }

  std::optional<std::string> parseString() {
    ++pos_; // opening quote
    std::string out;

    while (!atEnd()) {
      const char c = text_[pos_++];
      if (c == '"') {
        return out;
      }
      if (c != '\\') {
        out += c;
        continue;
      }
      if (atEnd()) {
        break;
      }
      switch (text_[pos_++]) {
      case '"':
        out += '"';
        break;
      case '\\':
        out += '\\';
        break;
      case '/':
        out += '/';
        break;
      case 'b':
        out += '\b';
        break;
      case 'f':
        out += '\f';
        break;
      case 'n':
        out += '\n';
        break;
      case 'r':
        out += '\r';
        break;
      case 't':
        out += '\t';
        break;
      default:
        // Including \u: rejected rather than guessed at.
        fail("unsupported escape sequence in string");
        return std::nullopt;
      }
    }

    fail("unterminated string");
    return std::nullopt;
  }

  std::optional<Value> parseNumber() {
    const std::size_t start = pos_;
    if (peek() == '-' || peek() == '+') {
      ++pos_;
    }
    while (!atEnd()) {
      const char c = text_[pos_];
      const bool part =
          (c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-';
      if (!part) {
        break;
      }
      ++pos_;
    }

    if (pos_ == start) {
      return fail("expected a value");
    }

    // std::from_chars for double is not available in every standard library we
    // care about, so this goes through strtod on a null-terminated copy. The
    // numbers here are short, so the allocation does not matter.
    const std::string text(text_.substr(start, pos_ - start));
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0') {
      pos_ = start;
      return fail("malformed number");
    }
    return Value(value);
  }

  std::string_view text_;
  std::string& error_;
  std::size_t pos_ = 0;
};

} // namespace

std::optional<Value> parse(std::string_view text, std::string& error) {
  error.clear();
  Parser parser(text, error);
  auto result = parser.parseDocument();
  if (!result && error.empty()) {
    error = "failed to parse JSON";
  }
  return result;
}

} // namespace mloptc::json
