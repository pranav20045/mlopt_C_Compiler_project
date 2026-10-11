#pragma once

#include "mloptc/diagnostics.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mloptc {

/// ToyC's type system, in its entirety (docs/spec.md section 3).
enum class Type : std::uint8_t {
  Int,   ///< a 32-bit two's-complement signed integer
  Void,  ///< only legal as a function return type
  Array, ///< int[N]; never a value, only a storage location
};

[[nodiscard]] std::string_view typeName(Type t) noexcept;

// =============================================================================
// Expressions
// =============================================================================

enum class ExprKind : std::uint8_t {
  IntLiteral,
  VarRef,
  ArraySubscript,
  Unary,
  Binary,
  Call,
};

enum class UnaryOp : std::uint8_t {
  Neg, ///< -x
  Not, ///< !x
};

enum class BinaryOp : std::uint8_t {
  Add,
  Sub,
  Mul,
  Div,
  Rem,
  Lt,
  Le,
  Gt,
  Ge,
  Eq,
  Ne,
  And, ///< && -- short-circuits, so it is NOT a normal binary operator
  Or,  ///< || -- likewise
};

[[nodiscard]] std::string_view unaryOpSymbol(UnaryOp op) noexcept;
[[nodiscard]] std::string_view binaryOpSymbol(BinaryOp op) noexcept;

/// Where a variable lives. Filled in by semantic analysis, not the parser --
/// the parser only sees a name, and cannot know what it refers to.
enum class VarScope : std::uint8_t {
  Global,
  Local, ///< includes parameters, which are just locals that start initialised
};

/// Base class for every expression.
///
/// Design: a tagged hierarchy (a `kind` field plus `static_cast`) rather than a
/// virtual visitor. This is what Clang itself does, and it has three advantages
/// here: switching on `kind` with -Wswitch means forgetting a case is a build
/// error; there is no visitor interface to keep in sync as the AST grows; and
/// it stays easy to read in a debugger. The virtual destructor is kept so that
/// `std::unique_ptr<Expr>` deletes derived objects correctly.
struct Expr {
  ExprKind kind;
  SourceLoc loc;

  /// Filled in by semantic analysis. Always Int for a well-typed expression --
  /// ToyC has no other value type -- but recording it makes the type checker's
  /// conclusions visible to later stages and to tests.
  Type type = Type::Int;

  Expr(ExprKind k, SourceLoc l) noexcept : kind(k), loc(l) {}

  Expr(const Expr&) = delete;
  Expr& operator=(const Expr&) = delete;
  virtual ~Expr() = default;
};

using ExprPtr = std::unique_ptr<Expr>;

struct IntLiteralExpr final : Expr {
  std::int32_t value = 0;

  IntLiteralExpr(std::int32_t v, SourceLoc l) noexcept : Expr(ExprKind::IntLiteral, l), value(v) {}
};

struct VarRefExpr final : Expr {
  std::string name;

  // Resolved by semantic analysis.
  VarScope scope = VarScope::Local;
  std::uint32_t index = 0;

  VarRefExpr(std::string n, SourceLoc l) : Expr(ExprKind::VarRef, l), name(std::move(n)) {}
};

struct ArraySubscriptExpr final : Expr {
  std::string name;
  ExprPtr index;

  // Resolved by semantic analysis.
  VarScope scope = VarScope::Local;
  std::uint32_t slot = 0;

  ArraySubscriptExpr(std::string n, ExprPtr idx, SourceLoc l)
      : Expr(ExprKind::ArraySubscript, l), name(std::move(n)), index(std::move(idx)) {}
};

struct UnaryExpr final : Expr {
  UnaryOp op;
  ExprPtr operand;

  UnaryExpr(UnaryOp o, ExprPtr e, SourceLoc l)
      : Expr(ExprKind::Unary, l), op(o), operand(std::move(e)) {}
};

struct BinaryExpr final : Expr {
  BinaryOp op;
  ExprPtr lhs;
  ExprPtr rhs;

  BinaryExpr(BinaryOp o, ExprPtr a, ExprPtr b, SourceLoc l)
      : Expr(ExprKind::Binary, l), op(o), lhs(std::move(a)), rhs(std::move(b)) {}
};

struct CallExpr final : Expr {
  std::string callee;
  std::vector<ExprPtr> args;

  /// Index into Program::functions, resolved by semantic analysis.
  std::uint32_t functionIndex = 0;

  CallExpr(std::string c, std::vector<ExprPtr> a, SourceLoc l)
      : Expr(ExprKind::Call, l), callee(std::move(c)), args(std::move(a)) {}
};

// =============================================================================
// Statements
// =============================================================================

enum class StmtKind : std::uint8_t {
  Block,
  VarDecl,
  Assign,
  ArrayAssign,
  If,
  While,
  For,
  Return,
  Break,
  Continue,
  ExprStmt,
  Print,
};

struct Stmt {
  StmtKind kind;
  SourceLoc loc;

  Stmt(StmtKind k, SourceLoc l) noexcept : kind(k), loc(l) {}

  Stmt(const Stmt&) = delete;
  Stmt& operator=(const Stmt&) = delete;
  virtual ~Stmt() = default;
};

using StmtPtr = std::unique_ptr<Stmt>;

struct BlockStmt final : Stmt {
  std::vector<StmtPtr> statements;

  explicit BlockStmt(SourceLoc l) : Stmt(StmtKind::Block, l) {}
};

struct VarDeclStmt final : Stmt {
  std::string name;
  bool isArray = false;
  std::int32_t arraySize = 0; ///< only when isArray
  ExprPtr init;               ///< may be null; the spec then defines the value as 0

  /// Slot within the enclosing function's frame, assigned by semantic analysis.
  std::uint32_t localIndex = 0;

  VarDeclStmt(std::string n, SourceLoc l) : Stmt(StmtKind::VarDecl, l), name(std::move(n)) {}
};

struct AssignStmt final : Stmt {
  std::string name;
  ExprPtr value;

  VarScope scope = VarScope::Local;
  std::uint32_t index = 0;

  AssignStmt(std::string n, ExprPtr v, SourceLoc l)
      : Stmt(StmtKind::Assign, l), name(std::move(n)), value(std::move(v)) {}
};

struct ArrayAssignStmt final : Stmt {
  std::string name;
  ExprPtr index;
  ExprPtr value;

  VarScope scope = VarScope::Local;
  std::uint32_t slot = 0;

  ArrayAssignStmt(std::string n, ExprPtr idx, ExprPtr v, SourceLoc l)
      : Stmt(StmtKind::ArrayAssign, l), name(std::move(n)), index(std::move(idx)),
        value(std::move(v)) {}
};

struct IfStmt final : Stmt {
  ExprPtr condition;
  StmtPtr thenBranch;
  StmtPtr elseBranch; ///< may be null

  explicit IfStmt(SourceLoc l) : Stmt(StmtKind::If, l) {}
};

struct WhileStmt final : Stmt {
  ExprPtr condition;
  StmtPtr body;

  explicit WhileStmt(SourceLoc l) : Stmt(StmtKind::While, l) {}
};

/// `for` is kept as its own node rather than desugared into `while` in the
/// parser. The reason is `continue`: per the spec it must still run the update
/// expression, which a naive desugaring into `while (cond) { body; update; }`
/// gets wrong -- `continue` would jump straight to the condition and skip the
/// update, turning `for (i = 0; i < n; i = i + 1)` into an infinite loop.
/// Keeping the node means each backend handles `continue` explicitly and
/// correctly.
struct ForStmt final : Stmt {
  StmtPtr init;   ///< may be null; a VarDeclStmt or an AssignStmt
  ExprPtr cond;   ///< may be null, meaning "always true"
  StmtPtr update; ///< may be null; an AssignStmt
  StmtPtr body;

  explicit ForStmt(SourceLoc l) : Stmt(StmtKind::For, l) {}
};

struct ReturnStmt final : Stmt {
  ExprPtr value; ///< null in a void function

  explicit ReturnStmt(SourceLoc l) : Stmt(StmtKind::Return, l) {}
};

struct BreakStmt final : Stmt {
  explicit BreakStmt(SourceLoc l) : Stmt(StmtKind::Break, l) {}
};

struct ContinueStmt final : Stmt {
  explicit ContinueStmt(SourceLoc l) : Stmt(StmtKind::Continue, l) {}
};

/// A bare expression evaluated for its side effects, e.g. a call: `f(1);`
struct ExprStmt final : Stmt {
  ExprPtr expr;

  ExprStmt(ExprPtr e, SourceLoc l) : Stmt(StmtKind::ExprStmt, l), expr(std::move(e)) {}
};

struct PrintStmt final : Stmt {
  ExprPtr value;

  PrintStmt(ExprPtr v, SourceLoc l) : Stmt(StmtKind::Print, l), value(std::move(v)) {}
};

// =============================================================================
// Declarations
// =============================================================================

struct Param {
  std::string name;
  bool isArray = false;
  std::uint32_t localIndex = 0;
  SourceLoc loc;
};

struct FunctionDecl {
  std::string name;
  Type returnType = Type::Int;
  std::vector<Param> params;
  std::unique_ptr<BlockStmt> body;
  SourceLoc loc;

  /// Total frame size, computed by semantic analysis: parameters plus every
  /// local declared anywhere inside the function. Shadowed and out-of-scope
  /// locals each get their own slot -- wasteful in frame space, but it means a
  /// frame is one flat vector and no scope bookkeeping survives into execution.
  std::uint32_t frameSize = 0;

  /// Index into Program::functions.
  std::uint32_t index = 0;
};

struct GlobalVarDecl {
  std::string name;
  bool isArray = false;
  std::int32_t arraySize = 0;
  std::int32_t initValue = 0; ///< the spec requires a constant initialiser
  std::uint32_t index = 0;
  SourceLoc loc;
};

struct Program {
  std::vector<GlobalVarDecl> globals;
  std::vector<std::unique_ptr<FunctionDecl>> functions;

  /// Index of `main`, resolved by semantic analysis.
  std::uint32_t mainIndex = 0;

  [[nodiscard]] const FunctionDecl* findFunction(std::string_view name) const noexcept;
};

/// Renders the AST back into an indented, source-like form. Used by tests (to
/// assert on structure without hand-writing comparisons) and by the CLI's
/// --emit-ast flag.
[[nodiscard]] std::string dumpAst(const Program& program);

} // namespace mloptc
