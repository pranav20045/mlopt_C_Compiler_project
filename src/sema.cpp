#include "mloptc/sema.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace mloptc {

namespace {

struct VarInfo {
  VarScope scope = VarScope::Local;
  std::uint32_t index = 0;
  bool isArray = false;
};

class Analyzer {
public:
  Analyzer(Program& program, DiagnosticEngine& diags) : program_(program), diags_(diags) {}

  bool run() {
    collectGlobals();
    collectFunctions();
    checkMain();

    for (auto& fn : program_.functions) {
      analyzeFunction(*fn);
    }

    return !diags_.hasErrors();
  }

private:
  // ---------------------------------------------------------------------------
  // Global tables
  // ---------------------------------------------------------------------------

  void collectGlobals() {
    for (std::uint32_t i = 0; i < program_.globals.size(); ++i) {
      GlobalVarDecl& g = program_.globals[i];
      g.index = i;
      if (globals_.count(g.name) != 0) {
        diags_.error(g.loc, "redefinition of global '" + g.name + "'");
        continue;
      }
      globals_[g.name] = VarInfo{VarScope::Global, i, g.isArray};
    }
  }

  void collectFunctions() {
    // Functions are collected before any body is analyzed, which is what makes
    // mutual recursion work without forward declarations: by the time we look
    // at a body, every function in the file is already known.
    for (std::uint32_t i = 0; i < program_.functions.size(); ++i) {
      FunctionDecl& fn = *program_.functions[i];
      fn.index = i;
      if (functions_.count(fn.name) != 0) {
        diags_.error(fn.loc, "redefinition of function '" + fn.name + "'");
        continue;
      }
      if (globals_.count(fn.name) != 0) {
        diags_.error(fn.loc, "'" + fn.name + "' is already declared as a global variable");
        continue;
      }
      functions_[fn.name] = i;
    }
  }

  void checkMain() {
    auto it = functions_.find("main");
    if (it == functions_.end()) {
      diags_.error(SourceLoc{}, "program has no 'main' function");
      return;
    }

    const FunctionDecl& main = *program_.functions[it->second];
    program_.mainIndex = it->second;

    if (main.returnType != Type::Int) {
      diags_.error(main.loc, "'main' must return int");
    }
    if (!main.params.empty()) {
      diags_.error(main.loc, "'main' must take no parameters");
    }
  }

  // ---------------------------------------------------------------------------
  // Scopes
  // ---------------------------------------------------------------------------

  void pushScope() { scopes_.emplace_back(); }

  void popScope() { scopes_.pop_back(); }

  /// Declares a local in the innermost scope. Shadowing an *outer* scope is
  /// legal; redeclaring in the *same* scope is not.
  std::uint32_t declareLocal(const std::string& name, bool isArray, SourceLoc loc) {
    if (!scopes_.empty() && scopes_.back().count(name) != 0) {
      diags_.error(loc, "redeclaration of '" + name + "' in the same scope");
    }
    const std::uint32_t slot = nextSlot_++;
    if (!scopes_.empty()) {
      scopes_.back()[name] = VarInfo{VarScope::Local, slot, isArray};
    }
    return slot;
  }

  /// Innermost-out lookup, falling back to globals.
  const VarInfo* lookup(const std::string& name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      auto found = it->find(name);
      if (found != it->end()) {
        return &found->second;
      }
    }
    auto found = globals_.find(name);
    return found != globals_.end() ? &found->second : nullptr;
  }

  // ---------------------------------------------------------------------------
  // Functions
  // ---------------------------------------------------------------------------

  void analyzeFunction(FunctionDecl& fn) {
    currentFn_ = &fn;
    nextSlot_ = 0;
    loopDepth_ = 0;
    scopes_.clear();
    pushScope();

    for (Param& p : fn.params) {
      p.localIndex = declareLocal(p.name, p.isArray, p.loc);
    }

    if (fn.body) {
      // The body's own block does not push an extra scope: parameters and
      // top-level locals share one scope, so `int a` shadowing a parameter `a`
      // is correctly an error rather than silent shadowing.
      for (StmtPtr& stmt : fn.body->statements) {
        analyzeStmt(*stmt);
      }
    }

    popScope();
    fn.frameSize = nextSlot_;
    currentFn_ = nullptr;
  }

  // ---------------------------------------------------------------------------
  // Statements
  // ---------------------------------------------------------------------------

  void analyzeStmt(Stmt& s) {
    switch (s.kind) {
    case StmtKind::Block: {
      auto& b = static_cast<BlockStmt&>(s);
      pushScope();
      for (StmtPtr& child : b.statements) {
        analyzeStmt(*child);
      }
      popScope();
      return;
    }

    case StmtKind::VarDecl: {
      auto& v = static_cast<VarDeclStmt&>(s);
      // The initialiser is analyzed BEFORE the name is declared, so
      // `int x = x;` reports "undeclared identifier" rather than silently
      // reading an uninitialised slot.
      if (v.init) {
        analyzeExpr(*v.init);
      }
      if (v.isArray && v.init) {
        diags_.error(v.loc, "an array declaration cannot have an initialiser");
      }
      v.localIndex = declareLocal(v.name, v.isArray, v.loc);
      return;
    }

    case StmtKind::Assign: {
      auto& a = static_cast<AssignStmt&>(s);
      analyzeExpr(*a.value);
      const VarInfo* info = lookup(a.name);
      if (info == nullptr) {
        diags_.error(a.loc, "assignment to undeclared variable '" + a.name + "'");
        return;
      }
      if (info->isArray) {
        diags_.error(a.loc, "cannot assign to array '" + a.name +
                                "' as a whole; assign to an element instead");
        return;
      }
      a.scope = info->scope;
      a.index = info->index;
      return;
    }

    case StmtKind::ArrayAssign: {
      auto& a = static_cast<ArrayAssignStmt&>(s);
      // Spec section 5.2: the index is evaluated before the right-hand side.
      // Analysis order does not change runtime order, but matching it keeps
      // error reporting in source order.
      analyzeExpr(*a.index);
      analyzeExpr(*a.value);
      const VarInfo* info = lookup(a.name);
      if (info == nullptr) {
        diags_.error(a.loc, "assignment to undeclared variable '" + a.name + "'");
        return;
      }
      if (!info->isArray) {
        diags_.error(a.loc, "'" + a.name + "' is not an array");
        return;
      }
      a.scope = info->scope;
      a.slot = info->index;
      return;
    }

    case StmtKind::If: {
      auto& i = static_cast<IfStmt&>(s);
      analyzeExpr(*i.condition);
      if (i.thenBranch) {
        analyzeStmt(*i.thenBranch);
      }
      if (i.elseBranch) {
        analyzeStmt(*i.elseBranch);
      }
      return;
    }

    case StmtKind::While: {
      auto& w = static_cast<WhileStmt&>(s);
      analyzeExpr(*w.condition);
      ++loopDepth_;
      if (w.body) {
        analyzeStmt(*w.body);
      }
      --loopDepth_;
      return;
    }

    case StmtKind::For: {
      auto& f = static_cast<ForStmt&>(s);
      // A variable declared in the init clause is scoped to the loop, so the
      // whole `for` gets its own scope.
      pushScope();
      if (f.init) {
        analyzeStmt(*f.init);
      }
      if (f.cond) {
        analyzeExpr(*f.cond);
      }
      if (f.update) {
        analyzeStmt(*f.update);
      }
      ++loopDepth_;
      if (f.body) {
        analyzeStmt(*f.body);
      }
      --loopDepth_;
      popScope();
      return;
    }

    case StmtKind::Return: {
      auto& r = static_cast<ReturnStmt&>(s);
      if (currentFn_ == nullptr) {
        return;
      }
      if (r.value) {
        analyzeExpr(*r.value);
        if (currentFn_->returnType == Type::Void) {
          diags_.error(r.loc,
                       "cannot return a value from void function '" + currentFn_->name + "'");
        }
      } else if (currentFn_->returnType == Type::Int) {
        diags_.error(r.loc, "non-void function '" + currentFn_->name + "' must return a value");
      }
      return;
    }

    case StmtKind::Break:
      if (loopDepth_ == 0) {
        diags_.error(s.loc, "'break' outside of a loop");
      }
      return;

    case StmtKind::Continue:
      if (loopDepth_ == 0) {
        diags_.error(s.loc, "'continue' outside of a loop");
      }
      return;

    case StmtKind::ExprStmt:
      analyzeExpr(*static_cast<ExprStmt&>(s).expr, /*allowVoid=*/true);
      return;

    case StmtKind::Print:
      analyzeExpr(*static_cast<PrintStmt&>(s).value);
      return;
    }
  }

  // ---------------------------------------------------------------------------
  // Expressions
  // ---------------------------------------------------------------------------

  /// `allowVoid` is true only at statement position, where calling a void
  /// function is the point rather than an error.
  void analyzeExpr(Expr& e, bool allowVoid = false) {
    switch (e.kind) {
    case ExprKind::IntLiteral:
      e.type = Type::Int;
      return;

    case ExprKind::VarRef: {
      auto& v = static_cast<VarRefExpr&>(e);
      const VarInfo* info = lookup(v.name);
      if (info == nullptr) {
        if (functions_.count(v.name) != 0) {
          diags_.error(v.loc, "'" + v.name + "' is a function; did you mean to call it?");
        } else {
          diags_.error(v.loc, "use of undeclared identifier '" + v.name + "'");
        }
        return;
      }
      if (info->isArray) {
        diags_.error(v.loc,
                     "array '" + v.name + "' is not a value; index it or pass it to a function");
        return;
      }
      v.scope = info->scope;
      v.index = info->index;
      v.type = Type::Int;
      return;
    }

    case ExprKind::ArraySubscript: {
      auto& a = static_cast<ArraySubscriptExpr&>(e);
      analyzeExpr(*a.index);
      const VarInfo* info = lookup(a.name);
      if (info == nullptr) {
        diags_.error(a.loc, "use of undeclared identifier '" + a.name + "'");
        return;
      }
      if (!info->isArray) {
        diags_.error(a.loc, "'" + a.name + "' is not an array");
        return;
      }
      a.scope = info->scope;
      a.slot = info->index;
      a.type = Type::Int;
      return;
    }

    case ExprKind::Unary: {
      auto& u = static_cast<UnaryExpr&>(e);
      analyzeExpr(*u.operand);
      u.type = Type::Int;
      return;
    }

    case ExprKind::Binary: {
      auto& b = static_cast<BinaryExpr&>(e);
      analyzeExpr(*b.lhs);
      analyzeExpr(*b.rhs);
      b.type = Type::Int;
      return;
    }

    case ExprKind::Call: {
      auto& c = static_cast<CallExpr&>(e);
      for (ExprPtr& arg : c.args) {
        analyzeArgument(*arg);
      }

      auto it = functions_.find(c.callee);
      if (it == functions_.end()) {
        diags_.error(c.loc, "call to undeclared function '" + c.callee + "'");
        return;
      }

      c.functionIndex = it->second;
      const FunctionDecl& callee = *program_.functions[it->second];

      if (c.args.size() != callee.params.size()) {
        diags_.error(c.loc, "function '" + c.callee + "' expects " +
                                std::to_string(callee.params.size()) + " argument(s), but " +
                                std::to_string(c.args.size()) + " given");
        return;
      }

      checkArrayArguments(c, callee);

      c.type = callee.returnType;
      if (callee.returnType == Type::Void && !allowVoid) {
        diags_.error(c.loc, "void function '" + c.callee + "' used where a value is required");
      }
      return;
    }
    }
  }

  /// Argument position is the ONE place a bare array name is legal, because
  /// passing an array is the only thing you can do with one besides indexing
  /// it (spec section 4.2). Analyzing it as an ordinary expression would hit
  /// the "array is not a value" rule and reject every valid array call, so the
  /// array case is recognised first and the expression rule applies to
  /// everything else.
  void analyzeArgument(Expr& e) {
    if (e.kind == ExprKind::VarRef) {
      auto& v = static_cast<VarRefExpr&>(e);
      if (const VarInfo* info = lookup(v.name); info != nullptr && info->isArray) {
        v.scope = info->scope;
        v.index = info->index;
        v.type = Type::Array; // the backends key off this to pass by handle
        return;
      }
    }
    analyzeExpr(e);
  }

  /// An `int[]` parameter must receive an array, and an `int` parameter must
  /// not. analyzeArgument has already typed each argument, so this is a direct
  /// comparison.
  void checkArrayArguments(const CallExpr& call, const FunctionDecl& callee) {
    for (std::size_t i = 0; i < call.args.size(); ++i) {
      const Expr& arg = *call.args[i];
      const bool wantArray = callee.params[i].isArray;
      const bool gotArray = arg.type == Type::Array;

      if (wantArray && !gotArray) {
        diags_.error(arg.loc, "argument " + std::to_string(i + 1) + " of '" + call.callee +
                                  "' must be an array");
      } else if (!wantArray && gotArray) {
        diags_.error(arg.loc, "argument " + std::to_string(i + 1) + " of '" + call.callee +
                                  "' must be an int, not an array");
      }
    }
  }

  Program& program_;
  DiagnosticEngine& diags_;

  std::unordered_map<std::string, VarInfo> globals_;
  std::unordered_map<std::string, std::uint32_t> functions_;
  std::vector<std::unordered_map<std::string, VarInfo>> scopes_;

  FunctionDecl* currentFn_ = nullptr;
  std::uint32_t nextSlot_ = 0;
  int loopDepth_ = 0;
};

} // namespace

bool analyze(Program& program, DiagnosticEngine& diags) {
  Analyzer analyzer(program, diags);
  return analyzer.run();
}

} // namespace mloptc
