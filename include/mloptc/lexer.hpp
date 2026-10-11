#pragma once

#include "mloptc/diagnostics.hpp"
#include "mloptc/token.hpp"

#include <cstddef>
#include <string_view>
#include <vector>

namespace mloptc {

/// Turns ToyC source text into a flat vector of tokens.
///
/// Design notes:
///
///  * The lexer produces the *whole* token vector up front rather than handing
///    the parser one token at a time. ToyC programs are small, and a vector is
///    far easier to debug (you can print it) and lets the parser peek ahead
///    arbitrarily without buffering machinery of its own.
///
///  * Lexing never fails. An illegal character is reported to the
///    DiagnosticEngine and emitted as `TokenKind::Unknown`, so the parser still
///    receives a well-formed stream and can report further errors in one run.
///
///  * LIFETIME: Token::lexeme is a std::string_view into the source buffer
///    passed to the constructor. That buffer must outlive every token produced.
class Lexer {
public:
  /// `source` must outlive this Lexer and all tokens it returns.
  Lexer(std::string_view source, DiagnosticEngine& diags) noexcept;

  /// Lexes the entire input. The returned vector always ends with exactly one
  /// EndOfFile token, so the parser never has to bounds-check.
  [[nodiscard]] std::vector<Token> tokenize();

private:
  [[nodiscard]] bool atEnd() const noexcept;

  /// Character at the current position (or '\0' past the end). `ahead` looks
  /// further forward without consuming.
  [[nodiscard]] char peek(std::size_t ahead = 0) const noexcept;

  /// Consumes one character, keeping line/column bookkeeping correct.
  char advance() noexcept;

  /// Consumes the next character only if it equals `expected`.
  [[nodiscard]] bool match(char expected) noexcept;

  void skipTrivia();

  [[nodiscard]] Token lexIdentifierOrKeyword();
  [[nodiscard]] Token lexIntLiteral();
  [[nodiscard]] Token lexOperator();

  /// Builds a token spanning [start, pos_), tagged with `startLoc`.
  [[nodiscard]] Token make(TokenKind kind, std::size_t start, SourceLoc startLoc) const noexcept;

  std::string_view src_;
  DiagnosticEngine& diags_;
  std::size_t pos_ = 0;
  SourceLoc loc_{};
};

} // namespace mloptc
