#include "mloptc/ast.hpp"

#include <sstream>

namespace mloptc {

std::string_view typeName(Type t) noexcept {
  switch (t) {
  case Type::Int:
    return "int";
  case Type::Void:
    return "void";
  case Type::Array:
    return "int[]";
  }
  return "<invalid>";
}

std::string_view unaryOpSymbol(UnaryOp op) noexcept {
  switch (op) {
  case UnaryOp::Neg:
    return "-";
  case UnaryOp::Not:
    return "!";
  }
  return "<invalid>";
}

std::string_view binaryOpSymbol(BinaryOp op) noexcept {
  switch (op) {
  case BinaryOp::Add:
    return "+";
  case BinaryOp::Sub:
    return "-";
  case BinaryOp::Mul:
    return "*";
  case BinaryOp::Div:
    return "/";
  case BinaryOp::Rem:
    return "%";
  case BinaryOp::Lt:
    return "<";
  case BinaryOp::Le:
    return "<=";
  case BinaryOp::Gt:
    return ">";
  case BinaryOp::Ge:
    return ">=";
  case BinaryOp::Eq:
    return "==";
  case BinaryOp::Ne:
    return "!=";
  case BinaryOp::And:
    return "&&";
  case BinaryOp::Or:
    return "||";
  }
  return "<invalid>";
}

const FunctionDecl* Program::findFunction(std::string_view name) const noexcept {
  for (const auto& fn : functions) {
    if (fn->name == name) {
      return fn.get();
    }
  }
  return nullptr;
}

namespace {

class AstPrinter {
public:
  explicit AstPrinter(std::ostringstream& os) : os_(os) {}

  void printProgram(const Program& program) {
    for (const GlobalVarDecl& g : program.globals) {
      indent();
      os_ << "global " << g.name;
      if (g.isArray) {
        os_ << '[' << g.arraySize << ']';
      } else {
        os_ << " = " << g.initValue;
      }
      os_ << "  @slot " << g.index << '\n';
    }

    for (const auto& fn : program.functions) {
      printFunction(*fn);
    }
  }

private:
  void indent() {
    for (int i = 0; i < depth_; ++i) {
      os_ << "  ";
    }
  }

  void printFunction(const FunctionDecl& fn) {
    indent();
    os_ << "function " << typeName(fn.returnType) << ' ' << fn.name << '(';
    for (std::size_t i = 0; i < fn.params.size(); ++i) {
      if (i > 0) {
        os_ << ", ";
      }
      os_ << fn.params[i].name;
      if (fn.params[i].isArray) {
        os_ << "[]";
      }
    }
    os_ << ")  frame=" << fn.frameSize << '\n';

    ++depth_;
    if (fn.body) {
      printStmt(*fn.body);
    }
    --depth_;
  }

  void printStmt(const Stmt& s) {
    indent();
    switch (s.kind) {
    case StmtKind::Block: {
      const auto& b = static_cast<const BlockStmt&>(s);
      os_ << "block\n";
      ++depth_;
      for (const StmtPtr& child : b.statements) {
        printStmt(*child);
      }
      --depth_;
      return;
    }
    case StmtKind::VarDecl: {
      const auto& v = static_cast<const VarDeclStmt&>(s);
      os_ << "var " << v.name;
      if (v.isArray) {
        os_ << '[' << v.arraySize << ']';
      }
      os_ << "  @slot " << v.localIndex << '\n';
      if (v.init) {
        ++depth_;
        printExpr(*v.init);
        --depth_;
      }
      return;
    }
    case StmtKind::Assign: {
      const auto& a = static_cast<const AssignStmt&>(s);
      os_ << "assign " << a.name << "  @" << scopeName(a.scope) << ' ' << a.index << '\n';
      ++depth_;
      printExpr(*a.value);
      --depth_;
      return;
    }
    case StmtKind::ArrayAssign: {
      const auto& a = static_cast<const ArrayAssignStmt&>(s);
      os_ << "assign " << a.name << "[]  @" << scopeName(a.scope) << ' ' << a.slot << '\n';
      ++depth_;
      printExpr(*a.index);
      printExpr(*a.value);
      --depth_;
      return;
    }
    case StmtKind::If: {
      const auto& i = static_cast<const IfStmt&>(s);
      os_ << "if\n";
      ++depth_;
      printExpr(*i.condition);
      printStmt(*i.thenBranch);
      if (i.elseBranch) {
        indent();
        os_ << "else\n";
        printStmt(*i.elseBranch);
      }
      --depth_;
      return;
    }
    case StmtKind::While: {
      const auto& w = static_cast<const WhileStmt&>(s);
      os_ << "while\n";
      ++depth_;
      printExpr(*w.condition);
      printStmt(*w.body);
      --depth_;
      return;
    }
    case StmtKind::For: {
      const auto& f = static_cast<const ForStmt&>(s);
      os_ << "for\n";
      ++depth_;
      if (f.init) {
        printStmt(*f.init);
      }
      if (f.cond) {
        printExpr(*f.cond);
      }
      if (f.update) {
        printStmt(*f.update);
      }
      printStmt(*f.body);
      --depth_;
      return;
    }
    case StmtKind::Return: {
      const auto& r = static_cast<const ReturnStmt&>(s);
      os_ << "return\n";
      if (r.value) {
        ++depth_;
        printExpr(*r.value);
        --depth_;
      }
      return;
    }
    case StmtKind::Break:
      os_ << "break\n";
      return;
    case StmtKind::Continue:
      os_ << "continue\n";
      return;
    case StmtKind::ExprStmt: {
      const auto& e = static_cast<const ExprStmt&>(s);
      os_ << "expr-stmt\n";
      ++depth_;
      printExpr(*e.expr);
      --depth_;
      return;
    }
    case StmtKind::Print: {
      const auto& p = static_cast<const PrintStmt&>(s);
      os_ << "print\n";
      ++depth_;
      printExpr(*p.value);
      --depth_;
      return;
    }
    }
  }

  void printExpr(const Expr& e) {
    indent();
    switch (e.kind) {
    case ExprKind::IntLiteral:
      os_ << "int " << static_cast<const IntLiteralExpr&>(e).value << '\n';
      return;
    case ExprKind::VarRef: {
      const auto& v = static_cast<const VarRefExpr&>(e);
      os_ << "ref " << v.name << "  @" << scopeName(v.scope) << ' ' << v.index << '\n';
      return;
    }
    case ExprKind::ArraySubscript: {
      const auto& a = static_cast<const ArraySubscriptExpr&>(e);
      os_ << "subscript " << a.name << "  @" << scopeName(a.scope) << ' ' << a.slot << '\n';
      ++depth_;
      printExpr(*a.index);
      --depth_;
      return;
    }
    case ExprKind::Unary: {
      const auto& u = static_cast<const UnaryExpr&>(e);
      os_ << "unary " << unaryOpSymbol(u.op) << '\n';
      ++depth_;
      printExpr(*u.operand);
      --depth_;
      return;
    }
    case ExprKind::Binary: {
      const auto& b = static_cast<const BinaryExpr&>(e);
      os_ << "binary " << binaryOpSymbol(b.op) << '\n';
      ++depth_;
      printExpr(*b.lhs);
      printExpr(*b.rhs);
      --depth_;
      return;
    }
    case ExprKind::Call: {
      const auto& c = static_cast<const CallExpr&>(e);
      os_ << "call " << c.callee << "  @fn " << c.functionIndex << '\n';
      ++depth_;
      for (const ExprPtr& arg : c.args) {
        printExpr(*arg);
      }
      --depth_;
      return;
    }
    }
  }

  static std::string_view scopeName(VarScope s) noexcept {
    switch (s) {
    case VarScope::Global:
      return "global";
    case VarScope::Local:
      return "local";
    }
    return "?";
  }

  std::ostringstream& os_;
  int depth_ = 0;
};

} // namespace

std::string dumpAst(const Program& program) {
  std::ostringstream os;
  AstPrinter printer(os);
  printer.printProgram(program);
  return os.str();
}

} // namespace mloptc
