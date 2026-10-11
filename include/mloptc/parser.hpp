#pragma once

#include "mloptc/ast.hpp"
#include "mloptc/diagnostics.hpp"
#include "mloptc/token.hpp"

#include <cstddef>
#include <vector>

namespace mloptc {

/// A recursive-descent parser for ToyC.
///
/// Why recursive descent rather than a parser generator? For a grammar this
/// small it is less code overall, it produces far better error messages (each
/// function knows exactly what it was looking for), and it is readable: the
/// call graph of the parser mirrors the grammar in docs/spec.md almost
/// line for line.
///
/// Error handling is "report and recover": a syntax error is recorded in the
/// DiagnosticEngine and the parser then skips to a plausible restart point
/// (usually the next `;` or `}`), so a single run can report many errors rather
/// than stopping at the first.
class Parser {
public:
  Parser(std::vector<Token> tokens, DiagnosticEngine& diags);

  /// Parses a whole translation unit. On error the returned Program may be
  /// incomplete -- always check `diags.hasErrors()` before using it.
  [[nodiscard]] Program parseProgram();

private:
  // --- token stream helpers ---
  [[nodiscard]] const Token& peek(std::size_t ahead = 0) const noexcept;
  [[nodiscard]] const Token& current() const noexcept;
  [[nodiscard]] bool check(TokenKind kind) const noexcept;
  [[nodiscard]] bool atEnd() const noexcept;
  const Token& advance() noexcept;
  bool match(TokenKind kind) noexcept;

  /// Consumes a token of the given kind, or reports `what` was expected.
  /// Returns false if the token did not match.
  bool expect(TokenKind kind, std::string_view what);

  void errorAt(const Token& tok, const std::string& message);

  /// Skips tokens until something that plausibly starts a new statement or
  /// declaration. Without this, one stray token produces a cascade of
  /// meaningless follow-on errors.
  void synchronize();

  // --- declarations ---
  void parseDeclaration(Program& program);
  std::unique_ptr<FunctionDecl> parseFunction(Type returnType, std::string name, SourceLoc loc);
  void parseGlobalVar(Program& program, std::string name, SourceLoc loc);

  // --- statements ---
  [[nodiscard]] std::unique_ptr<BlockStmt> parseBlock();
  [[nodiscard]] StmtPtr parseStatement();
  [[nodiscard]] StmtPtr parseVarDecl();
  [[nodiscard]] StmtPtr parseIf();
  [[nodiscard]] StmtPtr parseWhile();
  [[nodiscard]] StmtPtr parseFor();
  [[nodiscard]] StmtPtr parseReturn();
  [[nodiscard]] StmtPtr parsePrint();

  /// Parses an assignment or a bare expression statement. These share a prefix
  /// (an identifier), so they cannot be distinguished until after it.
  [[nodiscard]] StmtPtr parseSimpleStatement(bool consumeSemicolon);

  // --- expressions ---
  //
  // One function per precedence level, each calling the next-tighter one. The
  // chain mirrors the table in docs/spec.md section 6.
  [[nodiscard]] ExprPtr parseExpression();
  [[nodiscard]] ExprPtr parseLogicalOr();
  [[nodiscard]] ExprPtr parseLogicalAnd();
  [[nodiscard]] ExprPtr parseEquality();
  [[nodiscard]] ExprPtr parseComparison();
  [[nodiscard]] ExprPtr parseAdditive();
  [[nodiscard]] ExprPtr parseMultiplicative();
  [[nodiscard]] ExprPtr parseUnary();
  [[nodiscard]] ExprPtr parsePrimary();

  std::vector<Token> tokens_;
  DiagnosticEngine& diags_;
  std::size_t pos_ = 0;

  /// Guards against pathological recursion on deeply nested input such as
  /// "((((((...1...))))))" -- a hand-written recursive-descent parser will
  /// otherwise overflow the stack, which is a crash, not a diagnostic.
  int depth_ = 0;
  static constexpr int kMaxDepth = 200;
};

} // namespace mloptc
