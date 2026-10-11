#include "mloptc/parser.hpp"

#include <limits>
#include <string>
#include <utility>

namespace mloptc {

namespace {

/// RAII guard for the recursion-depth counter, so every early return and every
/// exception path decrements it exactly once.
class DepthGuard {
public:
  explicit DepthGuard(int& depth) noexcept : depth_(depth) { ++depth_; }

  ~DepthGuard() { --depth_; }

  DepthGuard(const DepthGuard&) = delete;
  DepthGuard& operator=(const DepthGuard&) = delete;

private:
  int& depth_;
};

} // namespace

Parser::Parser(std::vector<Token> tokens, DiagnosticEngine& diags)
    : tokens_(std::move(tokens)), diags_(diags) {}

// =============================================================================
// Token stream
// =============================================================================

const Token& Parser::peek(std::size_t ahead) const noexcept {
  const std::size_t idx = pos_ + ahead;
  // The lexer guarantees a trailing EndOfFile, so clamping here means we can
  // always return a valid token without the caller bounds-checking.
  return idx < tokens_.size() ? tokens_[idx] : tokens_.back();
}

const Token& Parser::current() const noexcept {
  return peek(0);
}

bool Parser::check(TokenKind kind) const noexcept {
  return current().kind == kind;
}

bool Parser::atEnd() const noexcept {
  return current().kind == TokenKind::EndOfFile;
}

const Token& Parser::advance() noexcept {
  const Token& tok = current();
  if (!atEnd()) {
    ++pos_;
  }
  return tok;
}

bool Parser::match(TokenKind kind) noexcept {
  if (check(kind)) {
    advance();
    return true;
  }
  return false;
}

void Parser::errorAt(const Token& tok, const std::string& message) {
  diags_.error(tok.loc, message);
}

bool Parser::expect(TokenKind kind, std::string_view what) {
  if (match(kind)) {
    return true;
  }
  errorAt(current(), "expected " + std::string(what) + ", found '" +
                         std::string(tokenKindName(current().kind)) + "'");
  return false;
}

void Parser::synchronize() {
  // Skip the offending token, then run forward until we reach something that
  // can plausibly begin a fresh statement. Semicolons and braces are the
  // natural anchors.
  if (!atEnd()) {
    advance();
  }

  while (!atEnd()) {
    if (tokens_[pos_ - 1].kind == TokenKind::Semicolon) {
      return;
    }
    switch (current().kind) {
    case TokenKind::KwInt:
    case TokenKind::KwVoid:
    case TokenKind::KwIf:
    case TokenKind::KwWhile:
    case TokenKind::KwFor:
    case TokenKind::KwReturn:
    case TokenKind::KwBreak:
    case TokenKind::KwContinue:
    case TokenKind::KwPrint:
    case TokenKind::RBrace:
      return;
    default:
      advance();
      break;
    }
  }
}

// =============================================================================
// Declarations
// =============================================================================

Program Parser::parseProgram() {
  Program program;

  while (!atEnd()) {
    const std::size_t before = pos_;
    parseDeclaration(program);

    // Defensive: if a sub-parser consumed nothing and reported nothing, we
    // would loop forever. Force progress.
    if (pos_ == before) {
      advance();
    }
  }

  for (std::uint32_t i = 0; i < program.functions.size(); ++i) {
    program.functions[i]->index = i;
  }
  for (std::uint32_t i = 0; i < program.globals.size(); ++i) {
    program.globals[i].index = i;
  }

  return program;
}

void Parser::parseDeclaration(Program& program) {
  const SourceLoc loc = current().loc;

  Type returnType = Type::Int;
  if (match(TokenKind::KwVoid)) {
    returnType = Type::Void;
  } else if (!match(TokenKind::KwInt)) {
    errorAt(current(), "expected a declaration ('int' or 'void'), found '" +
                           std::string(tokenKindName(current().kind)) + "'");
    synchronize();
    return;
  }

  if (!check(TokenKind::Identifier)) {
    errorAt(current(), "expected a name after '" + std::string(typeName(returnType)) + "'");
    synchronize();
    return;
  }
  std::string name(advance().lexeme);

  // The token after the name decides: '(' means a function, anything else a
  // global variable. One token of lookahead is all the grammar needs.
  if (check(TokenKind::LParen)) {
    auto fn = parseFunction(returnType, std::move(name), loc);
    if (fn) {
      program.functions.push_back(std::move(fn));
    }
    return;
  }

  if (returnType == Type::Void) {
    errorAt(current(), "a variable cannot have type 'void'");
    synchronize();
    return;
  }
  parseGlobalVar(program, std::move(name), loc);
}

void Parser::parseGlobalVar(Program& program, std::string name, SourceLoc loc) {
  GlobalVarDecl decl;
  decl.name = std::move(name);
  decl.loc = loc;

  if (match(TokenKind::LBracket)) {
    decl.isArray = true;
    if (check(TokenKind::IntLiteral)) {
      const Token& sizeTok = advance();
      if (sizeTok.intValue < 1 || sizeTok.intValue > std::numeric_limits<std::int32_t>::max()) {
        errorAt(sizeTok, "array size must be at least 1");
        decl.arraySize = 1;
      } else {
        decl.arraySize = static_cast<std::int32_t>(sizeTok.intValue);
      }
    } else {
      errorAt(current(), "expected an array size");
      decl.arraySize = 1;
    }
    expect(TokenKind::RBracket, "']'");
  } else if (match(TokenKind::Equal)) {
    // The spec requires a constant initialiser for globals. Rather than build a
    // general constant evaluator, we accept an optionally negated literal,
    // which is all any realistic ToyC program needs.
    bool negate = false;
    while (match(TokenKind::Minus)) {
      negate = !negate;
    }
    if (check(TokenKind::IntLiteral)) {
      const Token& tok = advance();
      std::int64_t value = tok.intValue;
      if (negate) {
        value = -value;
      }
      if (value < std::numeric_limits<std::int32_t>::min() ||
          value > std::numeric_limits<std::int32_t>::max()) {
        errorAt(tok, "global initialiser is out of range for int");
        value = 0;
      }
      decl.initValue = static_cast<std::int32_t>(value);
    } else {
      errorAt(current(), "a global initialiser must be a constant integer");
    }
  }

  expect(TokenKind::Semicolon, "';'");
  program.globals.push_back(std::move(decl));
}

std::unique_ptr<FunctionDecl> Parser::parseFunction(Type returnType, std::string name,
                                                    SourceLoc loc) {
  auto fn = std::make_unique<FunctionDecl>();
  fn->name = std::move(name);
  fn->returnType = returnType;
  fn->loc = loc;

  expect(TokenKind::LParen, "'('");

  if (!check(TokenKind::RParen)) {
    for (;;) {
      Param param;
      param.loc = current().loc;
      if (!expect(TokenKind::KwInt, "'int'")) {
        break;
      }
      if (!check(TokenKind::Identifier)) {
        errorAt(current(), "expected a parameter name");
        break;
      }
      param.name = std::string(advance().lexeme);

      if (match(TokenKind::LBracket)) {
        param.isArray = true;
        expect(TokenKind::RBracket, "']' (array parameters have no size)");
      }

      fn->params.push_back(std::move(param));

      if (!match(TokenKind::Comma)) {
        break;
      }
    }
  }

  expect(TokenKind::RParen, "')'");

  if (!check(TokenKind::LBrace)) {
    errorAt(current(), "expected '{' to begin the body of '" + fn->name + "'");
    synchronize();
    return fn;
  }

  fn->body = parseBlock();
  return fn;
}

// =============================================================================
// Statements
// =============================================================================

std::unique_ptr<BlockStmt> Parser::parseBlock() {
  auto block = std::make_unique<BlockStmt>(current().loc);
  expect(TokenKind::LBrace, "'{'");

  while (!check(TokenKind::RBrace) && !atEnd()) {
    const std::size_t before = pos_;
    StmtPtr stmt = parseStatement();
    if (stmt) {
      block->statements.push_back(std::move(stmt));
    }
    if (pos_ == before) {
      advance(); // guarantee progress
    }
  }

  expect(TokenKind::RBrace, "'}'");
  return block;
}

StmtPtr Parser::parseStatement() {
  if (depth_ >= kMaxDepth) {
    errorAt(current(), "statement nesting is too deep");
    synchronize();
    return nullptr;
  }
  const DepthGuard guard(depth_);

  switch (current().kind) {
  case TokenKind::LBrace:
    return parseBlock();
  case TokenKind::KwInt:
    return parseVarDecl();
  case TokenKind::KwIf:
    return parseIf();
  case TokenKind::KwWhile:
    return parseWhile();
  case TokenKind::KwFor:
    return parseFor();
  case TokenKind::KwReturn:
    return parseReturn();
  case TokenKind::KwPrint:
    return parsePrint();
  case TokenKind::KwBreak: {
    const SourceLoc loc = advance().loc;
    expect(TokenKind::Semicolon, "';'");
    return std::make_unique<BreakStmt>(loc);
  }
  case TokenKind::KwContinue: {
    const SourceLoc loc = advance().loc;
    expect(TokenKind::Semicolon, "';'");
    return std::make_unique<ContinueStmt>(loc);
  }
  case TokenKind::Semicolon:
    // An empty statement. Harmless; consume and produce nothing.
    advance();
    return nullptr;
  default:
    return parseSimpleStatement(/*consumeSemicolon=*/true);
  }
}

StmtPtr Parser::parseVarDecl() {
  const SourceLoc loc = current().loc;
  expect(TokenKind::KwInt, "'int'");

  if (!check(TokenKind::Identifier)) {
    errorAt(current(), "expected a variable name after 'int'");
    synchronize();
    return nullptr;
  }

  auto decl = std::make_unique<VarDeclStmt>(std::string(advance().lexeme), loc);

  if (match(TokenKind::LBracket)) {
    decl->isArray = true;
    if (check(TokenKind::IntLiteral)) {
      const Token& sizeTok = advance();
      if (sizeTok.intValue < 1 || sizeTok.intValue > std::numeric_limits<std::int32_t>::max()) {
        errorAt(sizeTok, "array size must be at least 1");
        decl->arraySize = 1;
      } else {
        decl->arraySize = static_cast<std::int32_t>(sizeTok.intValue);
      }
    } else {
      errorAt(current(), "expected an array size");
      decl->arraySize = 1;
    }
    expect(TokenKind::RBracket, "']'");
    // Diagnosed here rather than in semantic analysis: the grammar has no
    // production for an array initialiser, so without this the user gets a
    // baffling "expected ';', found '='" instead of the real problem.
    if (check(TokenKind::Equal)) {
      errorAt(current(), "an array declaration cannot have an initialiser");
      advance();
      // Parsed and discarded, so the rest of the file still makes sense.
      static_cast<void>(parseExpression());
    }
  } else if (match(TokenKind::Equal)) {
    decl->init = parseExpression();
  }

  expect(TokenKind::Semicolon, "';'");
  return decl;
}

StmtPtr Parser::parseIf() {
  auto stmt = std::make_unique<IfStmt>(current().loc);
  advance(); // 'if'

  expect(TokenKind::LParen, "'(' after 'if'");
  stmt->condition = parseExpression();
  expect(TokenKind::RParen, "')'");

  stmt->thenBranch = parseStatement();

  // A dangling `else` binds to the nearest unmatched `if`. Recursive descent
  // gets this for free: the innermost parseIf() still on the stack is the one
  // that sees the `else` token first.
  if (match(TokenKind::KwElse)) {
    stmt->elseBranch = parseStatement();
  }

  return stmt;
}

StmtPtr Parser::parseWhile() {
  auto stmt = std::make_unique<WhileStmt>(current().loc);
  advance(); // 'while'

  expect(TokenKind::LParen, "'(' after 'while'");
  stmt->condition = parseExpression();
  expect(TokenKind::RParen, "')'");

  stmt->body = parseStatement();
  return stmt;
}

StmtPtr Parser::parseFor() {
  auto stmt = std::make_unique<ForStmt>(current().loc);
  advance(); // 'for'

  expect(TokenKind::LParen, "'(' after 'for'");

  // init
  if (!match(TokenKind::Semicolon)) {
    if (check(TokenKind::KwInt)) {
      stmt->init = parseVarDecl(); // consumes its own ';'
    } else {
      stmt->init = parseSimpleStatement(/*consumeSemicolon=*/true);
    }
  }

  // condition
  if (!check(TokenKind::Semicolon)) {
    stmt->cond = parseExpression();
  }
  expect(TokenKind::Semicolon, "';'");

  // update -- note: no trailing semicolon, it is terminated by ')'
  if (!check(TokenKind::RParen)) {
    stmt->update = parseSimpleStatement(/*consumeSemicolon=*/false);
  }
  expect(TokenKind::RParen, "')'");

  stmt->body = parseStatement();
  return stmt;
}

StmtPtr Parser::parseReturn() {
  auto stmt = std::make_unique<ReturnStmt>(current().loc);
  advance(); // 'return'

  if (!check(TokenKind::Semicolon)) {
    stmt->value = parseExpression();
  }
  expect(TokenKind::Semicolon, "';'");
  return stmt;
}

StmtPtr Parser::parsePrint() {
  const SourceLoc loc = current().loc;
  advance(); // 'print'

  expect(TokenKind::LParen, "'(' after 'print'");
  ExprPtr value = parseExpression();
  expect(TokenKind::RParen, "')'");
  expect(TokenKind::Semicolon, "';'");

  if (!value) {
    return nullptr;
  }
  return std::make_unique<PrintStmt>(std::move(value), loc);
}

StmtPtr Parser::parseSimpleStatement(bool consumeSemicolon) {
  const SourceLoc loc = current().loc;

  // Assignment and expression statements both begin with an identifier, so we
  // look ahead to the token *after* the name (and after a `[...]` subscript) to
  // decide. ToyC assignment is a statement, not an expression, which is exactly
  // what makes this one-token decision possible.
  if (check(TokenKind::Identifier)) {
    if (peek(1).kind == TokenKind::Equal) {
      std::string name(advance().lexeme);
      advance(); // '='
      ExprPtr value = parseExpression();
      if (consumeSemicolon) {
        expect(TokenKind::Semicolon, "';'");
      }
      if (!value) {
        return nullptr;
      }
      return std::make_unique<AssignStmt>(std::move(name), std::move(value), loc);
    }

    if (peek(1).kind == TokenKind::LBracket) {
      // Could be `a[i] = v;` (assignment) or `a[i];` (a pointless but legal
      // expression statement). Parse the subscript, then look for '='.
      const std::size_t save = pos_;
      std::string name(advance().lexeme);
      advance(); // '['
      ExprPtr index = parseExpression();
      if (expect(TokenKind::RBracket, "']'") && check(TokenKind::Equal)) {
        advance(); // '='
        ExprPtr value = parseExpression();
        if (consumeSemicolon) {
          expect(TokenKind::Semicolon, "';'");
        }
        if (!index || !value) {
          return nullptr;
        }
        return std::make_unique<ArrayAssignStmt>(std::move(name), std::move(index),
                                                 std::move(value), loc);
      }
      // Not an assignment after all -- rewind and treat it as an expression.
      pos_ = save;
    }
  }

  ExprPtr expr = parseExpression();
  if (consumeSemicolon) {
    expect(TokenKind::Semicolon, "';'");
  }
  if (!expr) {
    return nullptr;
  }
  return std::make_unique<ExprStmt>(std::move(expr), loc);
}

// =============================================================================
// Expressions
// =============================================================================
//
// Each level handles one row of the precedence table and delegates to the next
// tighter level. Left-associativity falls out of the `while` loop: we fold the
// accumulated left-hand side into a new node on every iteration, so `a - b - c`
// becomes `(a - b) - c` rather than `a - (b - c)`.

ExprPtr Parser::parseExpression() {
  if (depth_ >= kMaxDepth) {
    errorAt(current(), "expression nesting is too deep");
    return nullptr;
  }
  const DepthGuard guard(depth_);
  return parseLogicalOr();
}

ExprPtr Parser::parseLogicalOr() {
  ExprPtr lhs = parseLogicalAnd();
  while (check(TokenKind::PipePipe)) {
    const SourceLoc loc = advance().loc;
    ExprPtr rhs = parseLogicalAnd();
    if (!lhs || !rhs) {
      return nullptr;
    }
    lhs = std::make_unique<BinaryExpr>(BinaryOp::Or, std::move(lhs), std::move(rhs), loc);
  }
  return lhs;
}

ExprPtr Parser::parseLogicalAnd() {
  ExprPtr lhs = parseEquality();
  while (check(TokenKind::AmpAmp)) {
    const SourceLoc loc = advance().loc;
    ExprPtr rhs = parseEquality();
    if (!lhs || !rhs) {
      return nullptr;
    }
    lhs = std::make_unique<BinaryExpr>(BinaryOp::And, std::move(lhs), std::move(rhs), loc);
  }
  return lhs;
}

ExprPtr Parser::parseEquality() {
  ExprPtr lhs = parseComparison();
  for (;;) {
    BinaryOp op{};
    if (check(TokenKind::EqualEqual)) {
      op = BinaryOp::Eq;
    } else if (check(TokenKind::BangEqual)) {
      op = BinaryOp::Ne;
    } else {
      return lhs;
    }
    const SourceLoc loc = advance().loc;
    ExprPtr rhs = parseComparison();
    if (!lhs || !rhs) {
      return nullptr;
    }
    lhs = std::make_unique<BinaryExpr>(op, std::move(lhs), std::move(rhs), loc);
  }
}

ExprPtr Parser::parseComparison() {
  ExprPtr lhs = parseAdditive();
  for (;;) {
    BinaryOp op{};
    switch (current().kind) {
    case TokenKind::Less:
      op = BinaryOp::Lt;
      break;
    case TokenKind::LessEqual:
      op = BinaryOp::Le;
      break;
    case TokenKind::Greater:
      op = BinaryOp::Gt;
      break;
    case TokenKind::GreaterEqual:
      op = BinaryOp::Ge;
      break;
    default:
      return lhs;
    }
    const SourceLoc loc = advance().loc;
    ExprPtr rhs = parseAdditive();
    if (!lhs || !rhs) {
      return nullptr;
    }
    lhs = std::make_unique<BinaryExpr>(op, std::move(lhs), std::move(rhs), loc);
  }
}

ExprPtr Parser::parseAdditive() {
  ExprPtr lhs = parseMultiplicative();
  for (;;) {
    BinaryOp op{};
    if (check(TokenKind::Plus)) {
      op = BinaryOp::Add;
    } else if (check(TokenKind::Minus)) {
      op = BinaryOp::Sub;
    } else {
      return lhs;
    }
    const SourceLoc loc = advance().loc;
    ExprPtr rhs = parseMultiplicative();
    if (!lhs || !rhs) {
      return nullptr;
    }
    lhs = std::make_unique<BinaryExpr>(op, std::move(lhs), std::move(rhs), loc);
  }
}

ExprPtr Parser::parseMultiplicative() {
  ExprPtr lhs = parseUnary();
  for (;;) {
    BinaryOp op{};
    if (check(TokenKind::Star)) {
      op = BinaryOp::Mul;
    } else if (check(TokenKind::Slash)) {
      op = BinaryOp::Div;
    } else if (check(TokenKind::Percent)) {
      op = BinaryOp::Rem;
    } else {
      return lhs;
    }
    const SourceLoc loc = advance().loc;
    ExprPtr rhs = parseUnary();
    if (!lhs || !rhs) {
      return nullptr;
    }
    lhs = std::make_unique<BinaryExpr>(op, std::move(lhs), std::move(rhs), loc);
  }
}

ExprPtr Parser::parseUnary() {
  if (depth_ >= kMaxDepth) {
    errorAt(current(), "expression nesting is too deep");
    return nullptr;
  }
  const DepthGuard guard(depth_);

  if (check(TokenKind::Minus) || check(TokenKind::Bang)) {
    const bool isNeg = check(TokenKind::Minus);
    const SourceLoc loc = advance().loc;

    // Special case: `-<literal>` is folded here rather than left as a negation
    // node. This is what lets the spec permit the literal 2147483648 as the
    // operand of a unary minus: -2147483648 is INT_MIN and fits, whereas
    // 2147483648 on its own does not.
    if (isNeg && check(TokenKind::IntLiteral)) {
      const Token& tok = advance();
      const std::int64_t value = -tok.intValue;
      if (value < std::numeric_limits<std::int32_t>::min()) {
        errorAt(tok, "integer literal is out of range for int");
        return std::make_unique<IntLiteralExpr>(0, loc);
      }
      return std::make_unique<IntLiteralExpr>(static_cast<std::int32_t>(value), loc);
    }

    ExprPtr operand = parseUnary();
    if (!operand) {
      return nullptr;
    }
    return std::make_unique<UnaryExpr>(isNeg ? UnaryOp::Neg : UnaryOp::Not, std::move(operand),
                                       loc);
  }

  return parsePrimary();
}

ExprPtr Parser::parsePrimary() {
  const SourceLoc loc = current().loc;

  if (check(TokenKind::IntLiteral)) {
    const Token& tok = advance();
    if (tok.intValue > std::numeric_limits<std::int32_t>::max()) {
      // Reachable only for exactly 2147483648 without a preceding minus; the
      // lexer already rejected anything larger.
      errorAt(tok, "integer literal is out of range for int "
                   "(2147483648 is only valid as '-2147483648')");
      return std::make_unique<IntLiteralExpr>(0, loc);
    }
    return std::make_unique<IntLiteralExpr>(static_cast<std::int32_t>(tok.intValue), loc);
  }

  if (check(TokenKind::LParen)) {
    advance();
    ExprPtr inner = parseExpression();
    expect(TokenKind::RParen, "')'");
    return inner;
  }

  if (check(TokenKind::Identifier)) {
    std::string name(advance().lexeme);

    if (match(TokenKind::LParen)) {
      std::vector<ExprPtr> args;
      if (!check(TokenKind::RParen)) {
        for (;;) {
          ExprPtr arg = parseExpression();
          if (arg) {
            args.push_back(std::move(arg));
          }
          if (!match(TokenKind::Comma)) {
            break;
          }
        }
      }
      expect(TokenKind::RParen, "')'");
      return std::make_unique<CallExpr>(std::move(name), std::move(args), loc);
    }

    if (match(TokenKind::LBracket)) {
      ExprPtr index = parseExpression();
      expect(TokenKind::RBracket, "']'");
      if (!index) {
        return nullptr;
      }
      return std::make_unique<ArraySubscriptExpr>(std::move(name), std::move(index), loc);
    }

    return std::make_unique<VarRefExpr>(std::move(name), loc);
  }

  if (check(TokenKind::KwPrint)) {
    errorAt(current(), "'print' is a statement, not an expression");
    synchronize();
    return nullptr;
  }

  errorAt(current(),
          "expected an expression, found '" + std::string(tokenKindName(current().kind)) + "'");
  if (!atEnd()) {
    advance();
  }
  return nullptr;
}

} // namespace mloptc
