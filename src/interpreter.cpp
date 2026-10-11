#include "mloptc/interpreter.hpp"

#include <string>
#include <vector>

namespace mloptc {

namespace {

/// How a statement finished. Returning this up the recursion is how `break`,
/// `continue` and `return` propagate without exceptions -- which matters
/// because they are ordinary control flow, not errors, and happen constantly.
enum class Flow : std::uint8_t {
  Normal,
  Break,
  Continue,
  Return,
  Trap, ///< a runtime error; unwinds all the way out
};

class Interpreter {
public:
  Interpreter(const Program& program, const InterpreterOptions& options)
      : program_(program), options_(options) {}

  ExecutionResult run() {
    initGlobals();

    if (status_ == RuntimeStatus::Ok) {
      std::int32_t exitValue = 0;
      callFunction(*program_.functions[program_.mainIndex], {}, exitValue);

      if (status_ == RuntimeStatus::Ok) {
        // Spec section 8: the exit status is main's return value mod 256.
        // C++'s % keeps the sign of the dividend, so a negative return would
        // give a negative status; the extra + 256) % 256 maps it into 0..255
        // the way a shell does.
        result_.exitStatus = static_cast<int>(((exitValue % 256) + 256) % 256);
      }
    }

    if (status_ != RuntimeStatus::Ok) {
      result_.exitStatus = status_ == RuntimeStatus::StepLimitExceeded ? kStepLimitExitStatus
                                                                       : kRuntimeErrorExitStatus;
    }
    result_.status = status_;
    result_.dynamicInstructions = steps_;
    return std::move(result_);
  }

private:
  // ---------------------------------------------------------------------------
  // Storage
  // ---------------------------------------------------------------------------
  //
  // A variable slot holds a plain int32. For an array, that int32 is a HANDLE:
  // an index into `arrays_`. This is what gives array parameters reference
  // semantics (spec section 4.2) -- passing an array copies the handle, not the
  // contents, so the callee writes through to the caller's array.

  void initGlobals() {
    globals_.assign(program_.globals.size(), 0);
    for (const GlobalVarDecl& g : program_.globals) {
      if (g.isArray) {
        globals_[g.index] = allocArray(g.arraySize);
      } else {
        globals_[g.index] = g.initValue;
      }
    }
  }

  std::int32_t allocArray(std::int32_t size) {
    arrays_.emplace_back(static_cast<std::size_t>(size), 0); // spec 7.4: zeroed
    return static_cast<std::int32_t>(arrays_.size() - 1);
  }

  std::vector<std::int32_t>& arrayFor(std::int32_t handle) {
    return arrays_[static_cast<std::size_t>(handle)];
  }

  // ---------------------------------------------------------------------------
  // Calls
  // ---------------------------------------------------------------------------

  Flow callFunction(const FunctionDecl& fn, const std::vector<std::int32_t>& args,
                    std::int32_t& returnValue) {
    if (depth_ >= kMaxCallDepth) {
      return trap(RuntimeStatus::CallDepthExceeded);
    }

    // Arrays declared inside this call are freed on exit by truncating the
    // arena back to its current size. Safe because arrays cannot escape: ToyC
    // has no way to return one or store a handle anywhere lasting.
    const std::size_t arenaMark = arrays_.size();
    const std::size_t frameMark = frame_.size();

    // Spec 7.4: every slot starts at zero.
    frame_.resize(frameMark + fn.frameSize, 0);
    for (std::size_t i = 0; i < args.size(); ++i) {
      frame_[frameMark + i] = args[i];
    }

    ++depth_;
    const std::size_t savedBase = base_;
    base_ = frameMark;

    returnValue = 0; // spec 5.4: falling off the end of an int function returns 0
    Flow flow = Flow::Normal;
    if (fn.body != nullptr) {
      for (const StmtPtr& stmt : fn.body->statements) {
        flow = execStmt(*stmt, returnValue);
        if (flow != Flow::Normal) {
          break;
        }
      }
    }

    base_ = savedBase;
    --depth_;
    frame_.resize(frameMark);
    arrays_.resize(arenaMark);

    if (flow == Flow::Trap) {
      return Flow::Trap;
    }
    return Flow::Normal;
  }

  Flow trap(RuntimeStatus s) {
    if (status_ == RuntimeStatus::Ok) {
      status_ = s;
    }
    return Flow::Trap;
  }

  /// Returns false once the step budget is exhausted.
  bool tick() {
    if (++steps_ > options_.maxSteps) {
      trap(RuntimeStatus::StepLimitExceeded);
      return false;
    }
    return true;
  }

  // ---------------------------------------------------------------------------
  // Statements
  // ---------------------------------------------------------------------------

  Flow execStmt(const Stmt& s, std::int32_t& returnValue) {
    if (!tick()) {
      return Flow::Trap;
    }

    switch (s.kind) {
    case StmtKind::Block: {
      const auto& b = static_cast<const BlockStmt&>(s);
      for (const StmtPtr& child : b.statements) {
        const Flow flow = execStmt(*child, returnValue);
        if (flow != Flow::Normal) {
          return flow;
        }
      }
      return Flow::Normal;
    }

    case StmtKind::VarDecl: {
      const auto& v = static_cast<const VarDeclStmt&>(s);
      if (v.isArray) {
        frame_[base_ + v.localIndex] = allocArray(v.arraySize);
        return Flow::Normal;
      }
      std::int32_t value = 0; // spec 7.4
      if (v.init) {
        if (!evalExpr(*v.init, value)) {
          return Flow::Trap;
        }
      }
      frame_[base_ + v.localIndex] = value;
      return Flow::Normal;
    }

    case StmtKind::Assign: {
      const auto& a = static_cast<const AssignStmt&>(s);
      std::int32_t value = 0;
      if (!evalExpr(*a.value, value)) {
        return Flow::Trap;
      }
      store(a.scope, a.index) = value;
      return Flow::Normal;
    }

    case StmtKind::ArrayAssign: {
      const auto& a = static_cast<const ArrayAssignStmt&>(s);
      // Spec 5.2: index first, then the right-hand side. Observable, because
      // either side may call a function that prints.
      std::int32_t index = 0;
      if (!evalExpr(*a.index, index)) {
        return Flow::Trap;
      }
      std::int32_t value = 0;
      if (!evalExpr(*a.value, value)) {
        return Flow::Trap;
      }

      std::vector<std::int32_t>& array = arrayFor(load(a.scope, a.slot));
      if (index < 0 || static_cast<std::size_t>(index) >= array.size()) {
        return trap(RuntimeStatus::IndexOutOfBounds);
      }
      array[static_cast<std::size_t>(index)] = value;
      return Flow::Normal;
    }

    case StmtKind::If: {
      const auto& i = static_cast<const IfStmt&>(s);
      std::int32_t cond = 0;
      if (!evalExpr(*i.condition, cond)) {
        return Flow::Trap;
      }
      if (cond != 0) {
        return i.thenBranch ? execStmt(*i.thenBranch, returnValue) : Flow::Normal;
      }
      return i.elseBranch ? execStmt(*i.elseBranch, returnValue) : Flow::Normal;
    }

    case StmtKind::While: {
      const auto& w = static_cast<const WhileStmt&>(s);
      for (;;) {
        if (!tick()) {
          return Flow::Trap;
        }
        std::int32_t cond = 0;
        if (!evalExpr(*w.condition, cond)) {
          return Flow::Trap;
        }
        if (cond == 0) {
          return Flow::Normal;
        }
        if (w.body) {
          const Flow flow = execStmt(*w.body, returnValue);
          if (flow == Flow::Break) {
            return Flow::Normal;
          }
          // Continue is absorbed here: it just means "go round again".
          if (flow == Flow::Return || flow == Flow::Trap) {
            return flow;
          }
        }
      }
    }

    case StmtKind::For: {
      const auto& f = static_cast<const ForStmt&>(s);
      if (f.init) {
        const Flow flow = execStmt(*f.init, returnValue);
        if (flow != Flow::Normal) {
          return flow;
        }
      }
      for (;;) {
        if (!tick()) {
          return Flow::Trap;
        }
        if (f.cond) {
          std::int32_t cond = 0;
          if (!evalExpr(*f.cond, cond)) {
            return Flow::Trap;
          }
          if (cond == 0) {
            return Flow::Normal;
          }
        }
        // An absent condition means "always true" (spec 5.3).

        if (f.body) {
          const Flow flow = execStmt(*f.body, returnValue);
          if (flow == Flow::Break) {
            return Flow::Normal;
          }
          if (flow == Flow::Return || flow == Flow::Trap) {
            return flow;
          }
          // Flow::Continue falls through to the update below -- which is the
          // whole reason `for` is a distinct AST node. Desugaring it into
          // `while (cond) { body; update; }` would let `continue` skip the
          // update and turn an ordinary counting loop into an infinite one.
        }

        if (f.update) {
          const Flow flow = execStmt(*f.update, returnValue);
          if (flow != Flow::Normal) {
            return flow;
          }
        }
      }
    }

    case StmtKind::Return: {
      const auto& r = static_cast<const ReturnStmt&>(s);
      if (r.value) {
        if (!evalExpr(*r.value, returnValue)) {
          return Flow::Trap;
        }
      }
      return Flow::Return;
    }

    case StmtKind::Break:
      return Flow::Break;

    case StmtKind::Continue:
      return Flow::Continue;

    case StmtKind::ExprStmt: {
      std::int32_t ignored = 0;
      if (!evalExpr(*static_cast<const ExprStmt&>(s).expr, ignored)) {
        return Flow::Trap;
      }
      return Flow::Normal;
    }

    case StmtKind::Print: {
      std::int32_t value = 0;
      if (!evalExpr(*static_cast<const PrintStmt&>(s).value, value)) {
        return Flow::Trap;
      }
      // std::to_string, not a stream: no locale, no thousands separators, and
      // '\n' explicitly rather than std::endl, so the byte sequence is
      // identical on every platform. The differential tester compares these
      // bytes exactly.
      result_.output += std::to_string(value);
      result_.output += '\n';
      return Flow::Normal;
    }
    }
    return Flow::Normal;
  }

  // ---------------------------------------------------------------------------
  // Storage access
  // ---------------------------------------------------------------------------

  std::int32_t& store(VarScope scope, std::uint32_t index) {
    return scope == VarScope::Global ? globals_[index] : frame_[base_ + index];
  }

  std::int32_t load(VarScope scope, std::uint32_t index) const {
    return scope == VarScope::Global ? globals_[index] : frame_[base_ + index];
  }

  // ---------------------------------------------------------------------------
  // Expressions
  // ---------------------------------------------------------------------------

  /// Returns false if a trap occurred; `out` is then meaningless.
  bool evalExpr(const Expr& e, std::int32_t& out) {
    if (!tick()) {
      return false;
    }

    switch (e.kind) {
    case ExprKind::IntLiteral:
      out = static_cast<const IntLiteralExpr&>(e).value;
      return true;

    case ExprKind::VarRef: {
      const auto& v = static_cast<const VarRefExpr&>(e);
      out = load(v.scope, v.index);
      return true;
    }

    case ExprKind::ArraySubscript: {
      const auto& a = static_cast<const ArraySubscriptExpr&>(e);
      std::int32_t index = 0;
      if (!evalExpr(*a.index, index)) {
        return false;
      }
      const std::vector<std::int32_t>& array = arrayFor(load(a.scope, a.slot));
      if (index < 0 || static_cast<std::size_t>(index) >= array.size()) {
        trap(RuntimeStatus::IndexOutOfBounds);
        return false;
      }
      out = array[static_cast<std::size_t>(index)];
      return true;
    }

    case ExprKind::Unary: {
      const auto& u = static_cast<const UnaryExpr&>(e);
      std::int32_t value = 0;
      if (!evalExpr(*u.operand, value)) {
        return false;
      }
      switch (u.op) {
      case UnaryOp::Neg:
        out = wrapNeg(value);
        return true;
      case UnaryOp::Not:
        out = value == 0 ? 1 : 0;
        return true;
      }
      return true;
    }

    case ExprKind::Binary:
      return evalBinary(static_cast<const BinaryExpr&>(e), out);

    case ExprKind::Call: {
      const auto& c = static_cast<const CallExpr&>(e);
      std::vector<std::int32_t> args;
      args.reserve(c.args.size());

      // Spec section 6: arguments evaluate left to right. In C this order is
      // unspecified; here it is pinned down, because an argument may print.
      for (const ExprPtr& arg : c.args) {
        std::int32_t value = 0;
        if (!evalArgument(*arg, value)) {
          return false;
        }
        args.push_back(value);
      }

      std::int32_t returnValue = 0;
      if (callFunction(*program_.functions[c.functionIndex], args, returnValue) == Flow::Trap) {
        return false;
      }
      out = returnValue;
      return true;
    }
    }
    return true;
  }

  /// An argument that is a bare array name passes its handle, giving reference
  /// semantics. Any other argument is an ordinary value.
  bool evalArgument(const Expr& arg, std::int32_t& out) {
    if (arg.kind == ExprKind::VarRef && arg.type == Type::Array) {
      const auto& v = static_cast<const VarRefExpr&>(arg);
      out = load(v.scope, v.index);
      return true;
    }
    return evalExpr(arg, out);
  }

  bool evalBinary(const BinaryExpr& b, std::int32_t& out) {
    // && and || must short-circuit (spec section 6), so they are handled before
    // either operand is touched. Treating them as ordinary binary operators
    // would evaluate the right-hand side unconditionally -- observable whenever
    // it contains a call that prints, and a correctness bug whenever the
    // left-hand side was guarding against a trap, as in `i < n && a[i] != 0`.
    if (b.op == BinaryOp::And) {
      std::int32_t lhs = 0;
      if (!evalExpr(*b.lhs, lhs)) {
        return false;
      }
      if (lhs == 0) {
        out = 0;
        return true;
      }
      std::int32_t rhs = 0;
      if (!evalExpr(*b.rhs, rhs)) {
        return false;
      }
      out = rhs != 0 ? 1 : 0;
      return true;
    }

    if (b.op == BinaryOp::Or) {
      std::int32_t lhs = 0;
      if (!evalExpr(*b.lhs, lhs)) {
        return false;
      }
      if (lhs != 0) {
        out = 1;
        return true;
      }
      std::int32_t rhs = 0;
      if (!evalExpr(*b.rhs, rhs)) {
        return false;
      }
      out = rhs != 0 ? 1 : 0;
      return true;
    }

    std::int32_t lhs = 0;
    std::int32_t rhs = 0;
    if (!evalExpr(*b.lhs, lhs)) { // spec: left operand fully evaluated first
      return false;
    }
    if (!evalExpr(*b.rhs, rhs)) {
      return false;
    }

    switch (b.op) {
    case BinaryOp::Add:
      out = wrapAdd(lhs, rhs);
      return true;
    case BinaryOp::Sub:
      out = wrapSub(lhs, rhs);
      return true;
    case BinaryOp::Mul:
      out = wrapMul(lhs, rhs);
      return true;
    case BinaryOp::Div: {
      RuntimeStatus s = RuntimeStatus::Ok;
      if (!checkedDiv(lhs, rhs, out, s)) {
        trap(s);
        return false;
      }
      return true;
    }
    case BinaryOp::Rem: {
      RuntimeStatus s = RuntimeStatus::Ok;
      if (!checkedRem(lhs, rhs, out, s)) {
        trap(s);
        return false;
      }
      return true;
    }
    case BinaryOp::Lt:
      out = lhs < rhs ? 1 : 0;
      return true;
    case BinaryOp::Le:
      out = lhs <= rhs ? 1 : 0;
      return true;
    case BinaryOp::Gt:
      out = lhs > rhs ? 1 : 0;
      return true;
    case BinaryOp::Ge:
      out = lhs >= rhs ? 1 : 0;
      return true;
    case BinaryOp::Eq:
      out = lhs == rhs ? 1 : 0;
      return true;
    case BinaryOp::Ne:
      out = lhs != rhs ? 1 : 0;
      return true;
    case BinaryOp::And:
    case BinaryOp::Or:
      return true; // handled above
    }
    return true;
  }

  const Program& program_;
  InterpreterOptions options_;

  ExecutionResult result_;
  RuntimeStatus status_ = RuntimeStatus::Ok;

  std::vector<std::int32_t> globals_;
  std::vector<std::int32_t> frame_; ///< one contiguous stack for all frames
  std::vector<std::vector<std::int32_t>> arrays_;

  std::size_t base_ = 0; ///< index in frame_ where the current frame starts
  std::uint32_t depth_ = 0;
  std::uint64_t steps_ = 0;
};

} // namespace

std::string_view runtimeStatusMessage(RuntimeStatus status) noexcept {
  switch (status) {
  case RuntimeStatus::Ok:
    return "ok";
  case RuntimeStatus::DivisionByZero:
    return "runtime error: division by zero";
  case RuntimeStatus::RemainderByZero:
    return "runtime error: remainder by zero";
  case RuntimeStatus::DivisionOverflow:
    return "runtime error: integer division overflow";
  case RuntimeStatus::IndexOutOfBounds:
    return "runtime error: array index out of bounds";
  case RuntimeStatus::CallDepthExceeded:
    return "runtime error: call depth exceeded";
  case RuntimeStatus::StepLimitExceeded:
    return "runtime error: step limit exceeded";
  }
  return "runtime error: unknown";
}

ExecutionResult interpret(const Program& program, const InterpreterOptions& options) {
  Interpreter interpreter(program, options);
  return interpreter.run();
}

} // namespace mloptc
