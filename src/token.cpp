#include "mloptc/token.hpp"

namespace mloptc {

std::string_view tokenKindName(TokenKind kind) noexcept {
  // A switch with no `default:` is deliberate. With -Wswitch -Werror, adding a
  // new TokenKind and forgetting it here is a compile error, not a mystery at
  // runtime. Every switch over an IR opcode later in the project follows the
  // same rule.
  switch (kind) {
  case TokenKind::EndOfFile:
    return "end of file";
  case TokenKind::Unknown:
    return "unknown character";
  case TokenKind::Identifier:
    return "identifier";
  case TokenKind::IntLiteral:
    return "integer literal";
  case TokenKind::KwInt:
    return "int";
  case TokenKind::KwVoid:
    return "void";
  case TokenKind::KwIf:
    return "if";
  case TokenKind::KwElse:
    return "else";
  case TokenKind::KwWhile:
    return "while";
  case TokenKind::KwFor:
    return "for";
  case TokenKind::KwReturn:
    return "return";
  case TokenKind::KwBreak:
    return "break";
  case TokenKind::KwContinue:
    return "continue";
  case TokenKind::KwPrint:
    return "print";
  case TokenKind::Plus:
    return "+";
  case TokenKind::Minus:
    return "-";
  case TokenKind::Star:
    return "*";
  case TokenKind::Slash:
    return "/";
  case TokenKind::Percent:
    return "%";
  case TokenKind::Less:
    return "<";
  case TokenKind::LessEqual:
    return "<=";
  case TokenKind::Greater:
    return ">";
  case TokenKind::GreaterEqual:
    return ">=";
  case TokenKind::EqualEqual:
    return "==";
  case TokenKind::BangEqual:
    return "!=";
  case TokenKind::AmpAmp:
    return "&&";
  case TokenKind::PipePipe:
    return "||";
  case TokenKind::Bang:
    return "!";
  case TokenKind::Equal:
    return "=";
  case TokenKind::LParen:
    return "(";
  case TokenKind::RParen:
    return ")";
  case TokenKind::LBrace:
    return "{";
  case TokenKind::RBrace:
    return "}";
  case TokenKind::LBracket:
    return "[";
  case TokenKind::RBracket:
    return "]";
  case TokenKind::Comma:
    return ",";
  case TokenKind::Semicolon:
    return ";";
  }
  return "<invalid>";
}

bool isKeyword(TokenKind kind) noexcept {
  switch (kind) {
  case TokenKind::KwInt:
  case TokenKind::KwVoid:
  case TokenKind::KwIf:
  case TokenKind::KwElse:
  case TokenKind::KwWhile:
  case TokenKind::KwFor:
  case TokenKind::KwReturn:
  case TokenKind::KwBreak:
  case TokenKind::KwContinue:
  case TokenKind::KwPrint:
    return true;
  default:
    return false;
  }
}

} // namespace mloptc
