#pragma once

#include "mloptc/diagnostics.hpp"

#include <cstdint>
#include <string_view>

namespace mloptc {

/// Every kind of token in ToyC (see docs/spec.md section 2).
///
/// This is an enum *class*, so `TokenKind::Plus` never silently converts to an
/// int. Combined with -Wswitch, forgetting a case anywhere becomes a build
/// error rather than a runtime surprise.
enum class TokenKind : std::uint8_t {
  // Sentinels
  EndOfFile,
  Unknown, // an illegal character; the lexer has already reported it

  // Literals and names
  Identifier,
  IntLiteral,

  // Keywords
  KwInt,
  KwVoid,
  KwIf,
  KwElse,
  KwWhile,
  KwFor,
  KwReturn,
  KwBreak,
  KwContinue,
  KwPrint,

  // Arithmetic
  Plus,
  Minus,
  Star,
  Slash,
  Percent,

  // Comparison
  Less,
  LessEqual,
  Greater,
  GreaterEqual,
  EqualEqual,
  BangEqual,

  // Logical
  AmpAmp,
  PipePipe,
  Bang,

  // Assignment
  Equal,

  // Punctuation
  LParen,
  RParen,
  LBrace,
  RBrace,
  LBracket,
  RBracket,
  Comma,
  Semicolon,
};

/// A short, human-readable name, used in parser error messages
/// ("expected ';', found 'int'").
[[nodiscard]] std::string_view tokenKindName(TokenKind kind) noexcept;

/// True for tokens that can begin an expression -- used by the parser for
/// error recovery.
[[nodiscard]] bool isKeyword(TokenKind kind) noexcept;

struct Token {
  TokenKind kind = TokenKind::EndOfFile;
  SourceLoc loc;

  /// The exact source text of this token. This is a *view* into the source
  /// buffer, so it costs nothing to copy -- but the source buffer must outlive
  /// every Token. The Lexer's contract documents this.
  std::string_view lexeme;

  /// Only meaningful when `kind == IntLiteral`.
  ///
  /// Held as int64 rather than int32 on purpose: the spec allows the literal
  /// 2147483648 to appear as the operand of a unary minus, and that value does
  /// not fit in an int32. The parser folds the minus and range-checks the
  /// result.
  std::int64_t intValue = 0;

  [[nodiscard]] bool is(TokenKind k) const noexcept { return kind == k; }
};

} // namespace mloptc
