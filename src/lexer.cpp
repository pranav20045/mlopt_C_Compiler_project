#include "mloptc/lexer.hpp"

#include <array>
#include <string>
#include <utility>

namespace mloptc {

namespace {

/// The largest value the lexer will accept. The spec allows 2147483648 to
/// appear as the operand of a unary minus (yielding INT_MIN); the parser
/// enforces that it appears nowhere else.
constexpr std::int64_t kMaxIntLiteral = 2147483648LL;

// We deliberately do NOT use std::isalpha / std::isdigit from <cctype>.
//
// Those take an `int` that must be representable as `unsigned char` or be EOF.
// Passing a plain `char` that happens to be negative -- which any byte >= 0x80
// is on platforms where char is signed, i.e. most of them -- is undefined
// behaviour. It is a real bug that shows up the moment someone puts a non-ASCII
// character in a source file. These three functions are also locale-independent,
// which matters because ToyC's lexical rules must not change with the user's
// locale.
constexpr bool isDigit(char c) noexcept {
  return c >= '0' && c <= '9';
}

constexpr bool isAlpha(char c) noexcept {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

constexpr bool isAlnum(char c) noexcept {
  return isAlpha(c) || isDigit(c);
}

/// Keyword table. Ten entries, so a linear scan beats a hash map: no allocation,
/// no hashing, and it fits in a cache line. Measure before complicating this.
constexpr std::array<std::pair<std::string_view, TokenKind>, 10> kKeywords{{
    {"int", TokenKind::KwInt},
    {"void", TokenKind::KwVoid},
    {"if", TokenKind::KwIf},
    {"else", TokenKind::KwElse},
    {"while", TokenKind::KwWhile},
    {"for", TokenKind::KwFor},
    {"return", TokenKind::KwReturn},
    {"break", TokenKind::KwBreak},
    {"continue", TokenKind::KwContinue},
    {"print", TokenKind::KwPrint},
}};

[[nodiscard]] TokenKind keywordOrIdentifier(std::string_view text) noexcept {
  for (const auto& [name, kind] : kKeywords) {
    if (name == text) {
      return kind;
    }
  }
  return TokenKind::Identifier;
}

/// Renders a byte for an error message, escaping anything unprintable so a
/// stray 0x01 does not corrupt the terminal.
[[nodiscard]] std::string describeChar(char c) {
  if (c >= 0x20 && c < 0x7F) {
    return std::string("'") + c + "'";
  }
  constexpr char kHex[] = "0123456789abcdef";
  const auto byte = static_cast<unsigned char>(c);
  std::string out = "'\\x";
  out += kHex[byte >> 4U];
  out += kHex[byte & 0x0FU];
  out += '\'';
  return out;
}

} // namespace

Lexer::Lexer(std::string_view source, DiagnosticEngine& diags) noexcept
    : src_(source), diags_(diags) {}

bool Lexer::atEnd() const noexcept {
  return pos_ >= src_.size();
}

char Lexer::peek(std::size_t ahead) const noexcept {
  const std::size_t idx = pos_ + ahead;
  return idx < src_.size() ? src_[idx] : '\0';
}

char Lexer::advance() noexcept {
  const char c = src_[pos_++];
  if (c == '\n') {
    ++loc_.line;
    loc_.column = 1;
  } else {
    ++loc_.column;
  }
  return c;
}

bool Lexer::match(char expected) noexcept {
  if (atEnd() || src_[pos_] != expected) {
    return false;
  }
  advance();
  return true;
}

void Lexer::skipTrivia() {
  // Loops because a comment can be followed by whitespace followed by another
  // comment, and we must consume all of it before the next real token.
  for (;;) {
    while (!atEnd() && (peek() == ' ' || peek() == '\t' || peek() == '\r' || peek() == '\n')) {
      advance();
    }

    if (peek() == '/' && peek(1) == '/') {
      while (!atEnd() && peek() != '\n') {
        advance();
      }
      continue;
    }

    if (peek() == '/' && peek(1) == '*') {
      const SourceLoc start = loc_;
      advance(); // '/'
      advance(); // '*'
      bool closed = false;
      while (!atEnd()) {
        if (peek() == '*' && peek(1) == '/') {
          advance();
          advance();
          closed = true;
          break;
        }
        advance();
      }
      if (!closed) {
        diags_.error(start, "unterminated block comment");
      }
      continue;
    }

    return;
  }
}

Token Lexer::make(TokenKind kind, std::size_t start, SourceLoc startLoc) const noexcept {
  Token tok;
  tok.kind = kind;
  tok.loc = startLoc;
  tok.lexeme = src_.substr(start, pos_ - start);
  return tok;
}

Token Lexer::lexIdentifierOrKeyword() {
  const std::size_t start = pos_;
  const SourceLoc startLoc = loc_;

  while (!atEnd() && isAlnum(peek())) {
    advance();
  }

  const std::string_view text = src_.substr(start, pos_ - start);
  return make(keywordOrIdentifier(text), start, startLoc);
}

Token Lexer::lexIntLiteral() {
  const std::size_t start = pos_;
  const SourceLoc startLoc = loc_;

  std::int64_t value = 0;
  bool overflowed = false;

  while (!atEnd() && isDigit(peek())) {
    const char c = advance();
    if (!overflowed) {
      value = value * 10 + static_cast<std::int64_t>(c - '0');
      // Stop accumulating as soon as we pass the limit, so `value` itself can
      // never overflow int64 no matter how many digits follow.
      if (value > kMaxIntLiteral) {
        overflowed = true;
      }
    }
  }

  // A literal immediately followed by a letter -- `123abc` -- is a lexical
  // error. Without this check we would silently produce IntLiteral(123)
  // followed by Identifier(abc), and the parser would report something
  // confusing instead.
  if (!atEnd() && isAlpha(peek())) {
    while (!atEnd() && isAlnum(peek())) {
      advance();
    }
    diags_.error(startLoc,
                 "invalid number '" + std::string(src_.substr(start, pos_ - start)) + "'");
    Token tok = make(TokenKind::IntLiteral, start, startLoc);
    tok.intValue = 0;
    return tok;
  }

  if (overflowed) {
    diags_.error(startLoc, "integer literal '" + std::string(src_.substr(start, pos_ - start)) +
                               "' is out of range for int");
    value = 0;
  }

  Token tok = make(TokenKind::IntLiteral, start, startLoc);
  tok.intValue = value;
  return tok;
}

Token Lexer::lexOperator() {
  const std::size_t start = pos_;
  const SourceLoc startLoc = loc_;
  const char c = advance();

  const auto emit = [&](TokenKind kind) { return make(kind, start, startLoc); };

  switch (c) {
  case '+':
    return emit(TokenKind::Plus);
  case '-':
    return emit(TokenKind::Minus);
  case '*':
    return emit(TokenKind::Star);
  case '/':
    return emit(TokenKind::Slash);
  case '%':
    return emit(TokenKind::Percent);
  case '(':
    return emit(TokenKind::LParen);
  case ')':
    return emit(TokenKind::RParen);
  case '{':
    return emit(TokenKind::LBrace);
  case '}':
    return emit(TokenKind::RBrace);
  case '[':
    return emit(TokenKind::LBracket);
  case ']':
    return emit(TokenKind::RBracket);
  case ',':
    return emit(TokenKind::Comma);
  case ';':
    return emit(TokenKind::Semicolon);

  // Two-character operators. `match` consumes the second character only if it
  // is the one we want, so '<' and '<=' are distinguished in one pass without
  // backtracking. This is called maximal munch: always take the longest token
  // that matches.
  case '<':
    return emit(match('=') ? TokenKind::LessEqual : TokenKind::Less);
  case '>':
    return emit(match('=') ? TokenKind::GreaterEqual : TokenKind::Greater);
  case '=':
    return emit(match('=') ? TokenKind::EqualEqual : TokenKind::Equal);
  case '!':
    return emit(match('=') ? TokenKind::BangEqual : TokenKind::Bang);

  // ToyC has no bitwise operators, so a lone '&' or '|' is always a typo for
  // the logical form. Saying so is much more useful than "unexpected character".
  case '&':
    if (match('&')) {
      return emit(TokenKind::AmpAmp);
    }
    diags_.error(startLoc, "unexpected '&'; ToyC has no bitwise operators (did you mean '&&'?)");
    return emit(TokenKind::Unknown);
  case '|':
    if (match('|')) {
      return emit(TokenKind::PipePipe);
    }
    diags_.error(startLoc, "unexpected '|'; ToyC has no bitwise operators (did you mean '||'?)");
    return emit(TokenKind::Unknown);

  default:
    diags_.error(startLoc, "unexpected character " + describeChar(c));
    return emit(TokenKind::Unknown);
  }
}

std::vector<Token> Lexer::tokenize() {
  std::vector<Token> tokens;

  for (;;) {
    skipTrivia();
    if (atEnd()) {
      break;
    }

    const char c = peek();
    if (isAlpha(c)) {
      tokens.push_back(lexIdentifierOrKeyword());
    } else if (isDigit(c)) {
      tokens.push_back(lexIntLiteral());
    } else {
      tokens.push_back(lexOperator());
    }
  }

  // Exactly one EndOfFile token, always. The parser relies on this: it can read
  // `tokens[i]` after any successful match without a bounds check, because the
  // stream cannot run out before it sees EndOfFile.
  Token eof;
  eof.kind = TokenKind::EndOfFile;
  eof.loc = loc_;
  eof.lexeme = {};
  tokens.push_back(eof);

  return tokens;
}

} // namespace mloptc
