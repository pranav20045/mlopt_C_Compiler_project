#include "mloptc/lowering.hpp"

#include <string>
#include <vector>

namespace mloptc {

namespace {

using namespace ir;

/// Where `break` and `continue` jump to, for the innermost enclosing loop.
///
/// `continueTarget` is the block holding the loop UPDATE for a `for`, and the
/// condition block for a `while`. Keeping it separate from the header is what
/// makes `continue` in a `for` loop run the update, as the spec requires.
struct LoopTargets {
  BlockId breakTarget = kNoBlock;
  BlockId continueTarget = kNoBlock;
};

class FunctionLowerer {
public:
  FunctionLowerer(const Program& program, const FunctionDecl& decl) : program_(program), decl_(decl) {}

  Function run() {
    fn_.name = decl_.name;
    fn_.returnType = decl_.returnType;
    fn_.numParams = static_cast<std::uint32_t>(decl_.params.size());
    fn_.frameSize = decl_.frameSize;

    current_ = newBlock("entry");
    fn_.entry = current_;

    if (decl_.body != nullptr) {
      for (const StmtPtr& stmt : decl_.body->statements) {
        lowerStmt(*stmt);
      }
    }

    // Every block must end in a terminator. A function that falls off its end
    // returns 0 (spec 5.4), and a void function just returns.
    if (!blockTerminated()) {
      if (fn_.returnType == Type::Int) {
        const ValueId zero = emitConst(0, decl_.loc);
        emitReturn(zero, decl_.loc);
      } else {
        emitReturn(kNoValue, decl_.loc);
      }
    }

    // Lowering creates blocks eagerly; some end up with no way in, for example
    // the code after an unconditional `return` inside a loop body.
    computeCfg(fn_);
    removeUnreachableBlocks(fn_);
    return std::move(fn_);
  }

private:
  // ---------------------------------------------------------------------------
  // Block and instruction emission
  // ---------------------------------------------------------------------------

  BlockId newBlock(std::string label) {
    const BlockId id = static_cast<BlockId>(fn_.blocks.size());
    BasicBlock block;
    block.id = id;
    block.label = std::move(label);
    fn_.blocks.push_back(std::move(block));
    return id;
  }

  BasicBlock& block() { return fn_.blocks[current_]; }

  bool blockTerminated() {
    const auto& insts = block().instructions;
    return !insts.empty() && isTerminator(insts.back().op);
  }

  /// Appends an instruction, unless the current block is already terminated.
  ///
  /// Silently dropping instructions after a terminator is what makes
  /// unreachable code (`return 1; print(2);`) harmless rather than producing a
  /// block with two terminators, which the verifier would reject.
  void emit(Instruction inst) {
    if (blockTerminated()) {
      return;
    }
    block().instructions.push_back(std::move(inst));
  }

  ValueId newValue() { return fn_.nextValue++; }

  /// Adds a frame slot not present in the source. Used for the result of a
  /// short-circuit expression, which needs somewhere to live across the merge
  /// of two control-flow paths -- the job a phi node would do in SSA form.
  std::uint32_t newTempSlot() { return fn_.frameSize++; }

  ValueId emitConst(std::int32_t value, SourceLoc loc) {
    Instruction inst;
    inst.op = Opcode::Const;
    inst.dest = newValue();
    inst.imm = value;
    inst.loc = loc;
    const ValueId dest = inst.dest;
    emit(std::move(inst));
    return dest;
  }

  ValueId emitBinary(Opcode op, ValueId a, ValueId b, SourceLoc loc) {
    Instruction inst;
    inst.op = op;
    inst.dest = newValue();
    inst.a = a;
    inst.b = b;
    inst.loc = loc;
    const ValueId dest = inst.dest;
    emit(std::move(inst));
    return dest;
  }

  ValueId emitUnary(Opcode op, ValueId a, SourceLoc loc) {
    Instruction inst;
    inst.op = op;
    inst.dest = newValue();
    inst.a = a;
    inst.loc = loc;
    const ValueId dest = inst.dest;
    emit(std::move(inst));
    return dest;
  }

  ValueId emitLoad(VarScope scope, std::uint32_t slot, SourceLoc loc) {
    Instruction inst;
    inst.op = scope == VarScope::Global ? Opcode::LoadGlobal : Opcode::LoadLocal;
    inst.dest = newValue();
    inst.imm = static_cast<std::int32_t>(slot);
    inst.loc = loc;
    const ValueId dest = inst.dest;
    emit(std::move(inst));
    return dest;
  }

  void emitStore(VarScope scope, std::uint32_t slot, ValueId value, SourceLoc loc) {
    Instruction inst;
    inst.op = scope == VarScope::Global ? Opcode::StoreGlobal : Opcode::StoreLocal;
    inst.imm = static_cast<std::int32_t>(slot);
    inst.a = value;
    inst.loc = loc;
    emit(std::move(inst));
  }

  void emitJump(BlockId target, SourceLoc loc) {
    Instruction inst;
    inst.op = Opcode::Jump;
    inst.targetTrue = target;
    inst.loc = loc;
    emit(std::move(inst));
  }

  void emitBranch(ValueId cond, BlockId ifTrue, BlockId ifFalse, SourceLoc loc) {
    Instruction inst;
    inst.op = Opcode::Branch;
    inst.a = cond;
    inst.targetTrue = ifTrue;
    inst.targetFalse = ifFalse;
    inst.loc = loc;
    emit(std::move(inst));
  }

  void emitReturn(ValueId value, SourceLoc loc) {
    Instruction inst;
    inst.op = Opcode::Return;
    inst.a = value;
    inst.loc = loc;
    emit(std::move(inst));
  }

  // ---------------------------------------------------------------------------
  // Statements
  // ---------------------------------------------------------------------------

  void lowerStmt(const Stmt& s) {
    switch (s.kind) {
    case StmtKind::Block:
      for (const StmtPtr& child : static_cast<const BlockStmt&>(s).statements) {
        lowerStmt(*child);
      }
      return;

    case StmtKind::VarDecl: {
      const auto& v = static_cast<const VarDeclStmt&>(s);
      if (v.isArray) {
        Instruction inst;
        inst.op = Opcode::Alloca;
        inst.dest = newValue();
        inst.imm = v.arraySize;
        inst.loc = v.loc;
        const ValueId handle = inst.dest;
        emit(std::move(inst));
        emitStore(VarScope::Local, v.localIndex, handle, v.loc);
        return;
      }
      // An uninitialised local is explicitly stored as 0. The frame is already
      // zeroed, but making it explicit means the IR alone is enough to know a
      // slot's value -- the optimizer never has to assume anything about how
      // the backend initialises frames.
      const ValueId value = v.init ? lowerExpr(*v.init) : emitConst(0, v.loc);
      emitStore(VarScope::Local, v.localIndex, value, v.loc);
      return;
    }

    case StmtKind::Assign: {
      const auto& a = static_cast<const AssignStmt&>(s);
      const ValueId value = lowerExpr(*a.value);
      emitStore(a.scope, a.index, value, a.loc);
      return;
    }

    case StmtKind::ArrayAssign: {
      const auto& a = static_cast<const ArrayAssignStmt&>(s);
      // Spec 5.2: index before value. The order these are emitted in IS the
      // order they execute in, so it has to match.
      const ValueId index = lowerExpr(*a.index);
      const ValueId value = lowerExpr(*a.value);
      const ValueId handle = emitLoad(a.scope, a.slot, a.loc);

      Instruction inst;
      inst.op = Opcode::ArrayStore;
      inst.a = handle;
      inst.b = index;
      inst.c = value;
      inst.loc = a.loc;
      emit(std::move(inst));
      return;
    }

    case StmtKind::If:
      lowerIf(static_cast<const IfStmt&>(s));
      return;

    case StmtKind::While:
      lowerWhile(static_cast<const WhileStmt&>(s));
      return;

    case StmtKind::For:
      lowerFor(static_cast<const ForStmt&>(s));
      return;

    case StmtKind::Return: {
      const auto& r = static_cast<const ReturnStmt&>(s);
      const ValueId value = r.value ? lowerExpr(*r.value) : kNoValue;
      emitReturn(value, r.loc);
      return;
    }

    case StmtKind::Break:
      if (!loops_.empty()) {
        emitJump(loops_.back().breakTarget, s.loc);
      }
      return;

    case StmtKind::Continue:
      if (!loops_.empty()) {
        emitJump(loops_.back().continueTarget, s.loc);
      }
      return;

    case StmtKind::ExprStmt:
      // Evaluated for effect; the result is discarded. DCE will remove the
      // computation if it turns out to be pure and unused.
      static_cast<void>(lowerExpr(*static_cast<const ExprStmt&>(s).expr));
      return;

    case StmtKind::Print: {
      const auto& p = static_cast<const PrintStmt&>(s);
      Instruction inst;
      inst.op = Opcode::Print;
      inst.a = lowerExpr(*p.value);
      inst.loc = p.loc;
      emit(std::move(inst));
      return;
    }
    }
  }

  void lowerIf(const IfStmt& s) {
    const ValueId cond = lowerExpr(*s.condition);

    const BlockId thenBlock = newBlock("if.then");
    const BlockId elseBlock = s.elseBranch ? newBlock("if.else") : kNoBlock;
    const BlockId endBlock = newBlock("if.end");

    emitBranch(cond, thenBlock, s.elseBranch ? elseBlock : endBlock, s.loc);

    current_ = thenBlock;
    if (s.thenBranch) {
      lowerStmt(*s.thenBranch);
    }
    if (!blockTerminated()) {
      emitJump(endBlock, s.loc);
    }

    if (s.elseBranch) {
      current_ = elseBlock;
      lowerStmt(*s.elseBranch);
      if (!blockTerminated()) {
        emitJump(endBlock, s.loc);
      }
    }

    current_ = endBlock;
  }

  void lowerWhile(const WhileStmt& s) {
    const BlockId condBlock = newBlock("while.cond");
    const BlockId bodyBlock = newBlock("while.body");
    const BlockId endBlock = newBlock("while.end");

    emitJump(condBlock, s.loc);

    current_ = condBlock;
    const ValueId cond = lowerExpr(*s.condition);
    emitBranch(cond, bodyBlock, endBlock, s.loc);

    current_ = bodyBlock;
    // For a `while`, continue goes back to the condition -- there is no update.
    loops_.push_back({endBlock, condBlock});
    if (s.body) {
      lowerStmt(*s.body);
    }
    loops_.pop_back();
    if (!blockTerminated()) {
      emitJump(condBlock, s.loc);
    }

    current_ = endBlock;
  }

  void lowerFor(const ForStmt& s) {
    if (s.init) {
      lowerStmt(*s.init);
    }

    const BlockId condBlock = newBlock("for.cond");
    const BlockId bodyBlock = newBlock("for.body");
    const BlockId updateBlock = newBlock("for.update");
    const BlockId endBlock = newBlock("for.end");

    emitJump(condBlock, s.loc);

    current_ = condBlock;
    if (s.cond) {
      const ValueId cond = lowerExpr(*s.cond);
      emitBranch(cond, bodyBlock, endBlock, s.loc);
    } else {
      emitJump(bodyBlock, s.loc); // an absent condition means "always true"
    }

    current_ = bodyBlock;
    // THE point of a separate update block: `continue` targets it, not the
    // condition, so the update still runs. Pointing continueTarget at
    // condBlock here would turn `for (i = 0; i < n; i = i + 1) { continue; }`
    // into an infinite loop.
    loops_.push_back({endBlock, updateBlock});
    if (s.body) {
      lowerStmt(*s.body);
    }
    loops_.pop_back();
    if (!blockTerminated()) {
      emitJump(updateBlock, s.loc);
    }

    current_ = updateBlock;
    if (s.update) {
      lowerStmt(*s.update);
    }
    if (!blockTerminated()) {
      emitJump(condBlock, s.loc);
    }

    current_ = endBlock;
  }

  // ---------------------------------------------------------------------------
  // Expressions
  // ---------------------------------------------------------------------------

  ValueId lowerExpr(const Expr& e) {
    switch (e.kind) {
    case ExprKind::IntLiteral:
      return emitConst(static_cast<const IntLiteralExpr&>(e).value, e.loc);

    case ExprKind::VarRef: {
      const auto& v = static_cast<const VarRefExpr&>(e);
      // Works for arrays too: the slot holds the handle, and passing the handle
      // is exactly what gives array arguments reference semantics.
      return emitLoad(v.scope, v.index, v.loc);
    }

    case ExprKind::ArraySubscript: {
      const auto& a = static_cast<const ArraySubscriptExpr&>(e);
      const ValueId index = lowerExpr(*a.index);
      const ValueId handle = emitLoad(a.scope, a.slot, a.loc);
      return emitBinary(Opcode::ArrayLoad, handle, index, a.loc);
    }

    case ExprKind::Unary: {
      const auto& u = static_cast<const UnaryExpr&>(e);
      const ValueId operand = lowerExpr(*u.operand);
      return emitUnary(u.op == UnaryOp::Neg ? Opcode::Neg : Opcode::Not, operand, u.loc);
    }

    case ExprKind::Binary:
      return lowerBinary(static_cast<const BinaryExpr&>(e));

    case ExprKind::Call: {
      const auto& c = static_cast<const CallExpr&>(e);

      Instruction inst;
      inst.op = Opcode::Call;
      inst.funcIndex = c.functionIndex;
      inst.loc = c.loc;
      // Spec section 6: arguments evaluate left to right, so they are lowered
      // in order. Emission order is execution order.
      for (const ExprPtr& arg : c.args) {
        inst.args.push_back(lowerExpr(*arg));
      }

      const FunctionDecl& callee = *program_.functions[c.functionIndex];
      if (callee.returnType == Type::Int) {
        inst.dest = newValue();
      }
      const ValueId dest = inst.dest;
      emit(std::move(inst));
      return dest;
    }
    }
    return kNoValue;
  }

  ValueId lowerBinary(const BinaryExpr& b) {
    if (b.op == BinaryOp::And || b.op == BinaryOp::Or) {
      return lowerShortCircuit(b);
    }

    // Spec section 6: the left operand is fully evaluated before the right.
    const ValueId lhs = lowerExpr(*b.lhs);
    const ValueId rhs = lowerExpr(*b.rhs);

    Opcode op = Opcode::Add;
    switch (b.op) {
    case BinaryOp::Add:
      op = Opcode::Add;
      break;
    case BinaryOp::Sub:
      op = Opcode::Sub;
      break;
    case BinaryOp::Mul:
      op = Opcode::Mul;
      break;
    case BinaryOp::Div:
      op = Opcode::Div;
      break;
    case BinaryOp::Rem:
      op = Opcode::Rem;
      break;
    case BinaryOp::Lt:
      op = Opcode::Lt;
      break;
    case BinaryOp::Le:
      op = Opcode::Le;
      break;
    case BinaryOp::Gt:
      op = Opcode::Gt;
      break;
    case BinaryOp::Ge:
      op = Opcode::Ge;
      break;
    case BinaryOp::Eq:
      op = Opcode::Eq;
      break;
    case BinaryOp::Ne:
      op = Opcode::Ne;
      break;
    case BinaryOp::And:
    case BinaryOp::Or:
      break; // handled above
    }

    return emitBinary(op, lhs, rhs, b.loc);
  }

  /// `a && b` and `a || b` are control flow, not arithmetic.
  ///
  /// Without phi nodes, the two paths need a shared place to leave their
  /// answer, so lowering allocates a frame slot. In an SSA IR this would be a
  /// phi node at the merge point; a memory slot is the non-SSA equivalent, and
  /// it is why mem2reg exists in compilers that do use SSA.
  ValueId lowerShortCircuit(const BinaryExpr& b) {
    const bool isAnd = b.op == BinaryOp::And;
    const std::uint32_t slot = newTempSlot();

    const BlockId rhsBlock = newBlock(isAnd ? "and.rhs" : "or.rhs");
    const BlockId endBlock = newBlock(isAnd ? "and.end" : "or.end");

    const ValueId lhs = lowerExpr(*b.lhs);

    // Seed the slot with the short-circuit answer: 0 for &&, 1 for ||. If we
    // take the shortcut, that value is already correct and nothing else runs.
    const ValueId seed = emitConst(isAnd ? 0 : 1, b.loc);
    emitStore(VarScope::Local, slot, seed, b.loc);

    if (isAnd) {
      emitBranch(lhs, rhsBlock, endBlock, b.loc);
    } else {
      emitBranch(lhs, endBlock, rhsBlock, b.loc);
    }

    current_ = rhsBlock;
    const ValueId rhs = lowerExpr(*b.rhs);
    // Normalise to 0 or 1: `2 && 3` is 1, not 3 (spec section 6).
    const ValueId zero = emitConst(0, b.loc);
    const ValueId normalised = emitBinary(Opcode::Ne, rhs, zero, b.loc);
    emitStore(VarScope::Local, slot, normalised, b.loc);
    if (!blockTerminated()) {
      emitJump(endBlock, b.loc);
    }

    current_ = endBlock;
    return emitLoad(VarScope::Local, slot, b.loc);
  }

  const Program& program_;
  const FunctionDecl& decl_;

  Function fn_;
  BlockId current_ = 0;
  std::vector<LoopTargets> loops_;
};

} // namespace

ir::Module lower(const Program& program) {
  ir::Module module;
  module.globals = program.globals;
  module.mainIndex = program.mainIndex;

  module.functions.reserve(program.functions.size());
  for (const auto& decl : program.functions) {
    FunctionLowerer lowerer(program, *decl);
    module.functions.push_back(lowerer.run());
  }

  return module;
}

} // namespace mloptc
