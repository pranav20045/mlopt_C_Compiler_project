#include "mloptc/ir.hpp"

#include <algorithm>
#include <sstream>
#include <unordered_map>

namespace mloptc::ir {

std::string_view opcodeName(Opcode op) noexcept {
  switch (op) {
  case Opcode::Const:
    return "const";
  case Opcode::Move:
    return "move";
  case Opcode::Add:
    return "add";
  case Opcode::Sub:
    return "sub";
  case Opcode::Mul:
    return "mul";
  case Opcode::Div:
    return "div";
  case Opcode::Rem:
    return "rem";
  case Opcode::Lt:
    return "lt";
  case Opcode::Le:
    return "le";
  case Opcode::Gt:
    return "gt";
  case Opcode::Ge:
    return "ge";
  case Opcode::Eq:
    return "eq";
  case Opcode::Ne:
    return "ne";
  case Opcode::Neg:
    return "neg";
  case Opcode::Not:
    return "not";
  case Opcode::LoadLocal:
    return "load.local";
  case Opcode::StoreLocal:
    return "store.local";
  case Opcode::LoadGlobal:
    return "load.global";
  case Opcode::StoreGlobal:
    return "store.global";
  case Opcode::Alloca:
    return "alloca";
  case Opcode::ArrayLoad:
    return "array.load";
  case Opcode::ArrayStore:
    return "array.store";
  case Opcode::Call:
    return "call";
  case Opcode::Print:
    return "print";
  case Opcode::Jump:
    return "jump";
  case Opcode::Branch:
    return "branch";
  case Opcode::Return:
    return "ret";
  }
  return "<invalid>";
}

bool isTerminator(Opcode op) noexcept {
  return op == Opcode::Jump || op == Opcode::Branch || op == Opcode::Return;
}

bool hasResult(Opcode op) noexcept {
  switch (op) {
  case Opcode::Const:
  case Opcode::Move:
  case Opcode::Add:
  case Opcode::Sub:
  case Opcode::Mul:
  case Opcode::Div:
  case Opcode::Rem:
  case Opcode::Lt:
  case Opcode::Le:
  case Opcode::Gt:
  case Opcode::Ge:
  case Opcode::Eq:
  case Opcode::Ne:
  case Opcode::Neg:
  case Opcode::Not:
  case Opcode::LoadLocal:
  case Opcode::LoadGlobal:
  case Opcode::Alloca:
  case Opcode::ArrayLoad:
    return true;
  case Opcode::Call:
    return true; // a void call still has dest == kNoValue; checked separately
  case Opcode::StoreLocal:
  case Opcode::StoreGlobal:
  case Opcode::ArrayStore:
  case Opcode::Print:
  case Opcode::Jump:
  case Opcode::Branch:
  case Opcode::Return:
    return false;
  }
  return false;
}

bool mayHaveSideEffects(Opcode op) noexcept {
  switch (op) {
  // Obviously effectful: they write memory, produce output, or transfer
  // control.
  case Opcode::StoreLocal:
  case Opcode::StoreGlobal:
  case Opcode::ArrayStore:
  case Opcode::Print:
  case Opcode::Call:
  case Opcode::Jump:
  case Opcode::Branch:
  case Opcode::Return:
  case Opcode::Alloca:
    return true;

  // The subtle ones. These produce a value, look pure, and ARE NOT.
  //
  // Div and Rem trap on a zero divisor and on INT_MIN / -1. ArrayLoad traps on
  // an out-of-bounds index. If DCE deleted `x = a / b` because x was unused,
  // a program that should have exited with status 70 would instead run to
  // completion -- a miscompilation every bit as real as computing the wrong
  // number, and one that a test suite of only well-behaved programs would
  // never catch.
  case Opcode::Div:
  case Opcode::Rem:
  case Opcode::ArrayLoad:
    return true;

  // Genuinely pure.
  case Opcode::Const:
  case Opcode::Move:
  case Opcode::Add:
  case Opcode::Sub:
  case Opcode::Mul:
  case Opcode::Lt:
  case Opcode::Le:
  case Opcode::Gt:
  case Opcode::Ge:
  case Opcode::Eq:
  case Opcode::Ne:
  case Opcode::Neg:
  case Opcode::Not:
    return false;

  // Loads are not effectful, but their result depends on memory, so CSE must
  // not reuse them across a store. isPure() excludes them for that reason.
  case Opcode::LoadLocal:
  case Opcode::LoadGlobal:
    return false;
  }
  return true;
}

bool isPure(Opcode op) noexcept {
  switch (op) {
  case Opcode::Add:
  case Opcode::Sub:
  case Opcode::Mul:
  case Opcode::Lt:
  case Opcode::Le:
  case Opcode::Gt:
  case Opcode::Ge:
  case Opcode::Eq:
  case Opcode::Ne:
  case Opcode::Neg:
  case Opcode::Not:
    return true;
  default:
    // Const and Move are excluded deliberately: they are already as cheap as
    // anything CSE could replace them with, and constant folding plus copy
    // propagation handle them better.
    return false;
  }
}

bool isCommutative(Opcode op) noexcept {
  switch (op) {
  case Opcode::Add:
  case Opcode::Mul:
  case Opcode::Eq:
  case Opcode::Ne:
    return true;
  default:
    return false;
  }
}

std::size_t Function::instructionCount() const noexcept {
  std::size_t total = 0;
  for (const BasicBlock& block : blocks) {
    total += block.instructions.size();
  }
  return total;
}

// =============================================================================
// CFG
// =============================================================================

void computeCfg(Function& fn) {
  for (BasicBlock& block : fn.blocks) {
    block.predecessors.clear();
    block.successors.clear();
  }

  for (BasicBlock& block : fn.blocks) {
    const Instruction* term = block.terminator();
    if (term == nullptr) {
      continue; // malformed; the verifier reports it
    }

    switch (term->op) {
    case Opcode::Jump:
      block.successors.push_back(term->targetTrue);
      break;
    case Opcode::Branch:
      block.successors.push_back(term->targetTrue);
      // A branch whose arms are the same block has ONE successor, not two.
      // Recording it twice would make the block appear twice in the
      // predecessor list and double-count edges in the ML features.
      if (term->targetFalse != term->targetTrue) {
        block.successors.push_back(term->targetFalse);
      }
      break;
    case Opcode::Return:
      break;
    default:
      break;
    }
  }

  for (BasicBlock& block : fn.blocks) {
    for (const BlockId succ : block.successors) {
      if (succ < fn.blocks.size()) {
        fn.blocks[succ].predecessors.push_back(block.id);
      }
    }
  }
}

bool removeUnreachableBlocks(Function& fn) {
  if (fn.blocks.empty()) {
    return false;
  }

  std::vector<bool> reachable(fn.blocks.size(), false);
  std::vector<BlockId> worklist{fn.entry};
  reachable[fn.entry] = true;

  while (!worklist.empty()) {
    const BlockId id = worklist.back();
    worklist.pop_back();

    const Instruction* term = fn.blocks[id].terminator();
    if (term == nullptr) {
      continue;
    }

    const auto visit = [&](BlockId succ) {
      if (succ != kNoBlock && succ < fn.blocks.size() && !reachable[succ]) {
        reachable[succ] = true;
        worklist.push_back(succ);
      }
    };

    if (term->op == Opcode::Jump) {
      visit(term->targetTrue);
    } else if (term->op == Opcode::Branch) {
      visit(term->targetTrue);
      visit(term->targetFalse);
    }
  }

  if (std::all_of(reachable.begin(), reachable.end(), [](bool b) { return b; })) {
    return false;
  }

  // Build old id -> new id, keeping the surviving blocks in their original
  // relative order so the IR dump stays readable.
  std::vector<BlockId> remap(fn.blocks.size(), kNoBlock);
  BlockId next = 0;
  for (BlockId i = 0; i < fn.blocks.size(); ++i) {
    if (reachable[i]) {
      remap[i] = next++;
    }
  }

  std::vector<BasicBlock> kept;
  kept.reserve(next);
  for (BlockId i = 0; i < fn.blocks.size(); ++i) {
    if (!reachable[i]) {
      continue;
    }
    BasicBlock block = std::move(fn.blocks[i]);
    block.id = remap[i];
    for (Instruction& inst : block.instructions) {
      if (inst.targetTrue != kNoBlock) {
        inst.targetTrue = remap[inst.targetTrue];
      }
      if (inst.targetFalse != kNoBlock) {
        inst.targetFalse = remap[inst.targetFalse];
      }
    }
    kept.push_back(std::move(block));
  }

  fn.blocks = std::move(kept);
  fn.entry = remap[fn.entry];
  computeCfg(fn);
  return true;
}

// =============================================================================
// Printing
// =============================================================================

namespace {

void printValue(std::ostringstream& os, ValueId id) {
  if (id == kNoValue) {
    os << "<none>";
  } else {
    os << '%' << id;
  }
}

void printInstruction(std::ostringstream& os, const Instruction& inst) {
  os << "    ";

  if (hasResult(inst.op) && inst.dest != kNoValue) {
    printValue(os, inst.dest);
    os << " = ";
  }

  os << opcodeName(inst.op);

  switch (inst.op) {
  case Opcode::Const:
    os << ' ' << inst.imm;
    break;

  case Opcode::Move:
  case Opcode::Neg:
  case Opcode::Not:
  case Opcode::Print:
    os << ' ';
    printValue(os, inst.a);
    break;

  case Opcode::Add:
  case Opcode::Sub:
  case Opcode::Mul:
  case Opcode::Div:
  case Opcode::Rem:
  case Opcode::Lt:
  case Opcode::Le:
  case Opcode::Gt:
  case Opcode::Ge:
  case Opcode::Eq:
  case Opcode::Ne:
  case Opcode::ArrayLoad:
    os << ' ';
    printValue(os, inst.a);
    os << ", ";
    printValue(os, inst.b);
    break;

  case Opcode::ArrayStore:
    os << ' ';
    printValue(os, inst.a);
    os << '[';
    printValue(os, inst.b);
    os << "] <- ";
    printValue(os, inst.c);
    break;

  case Opcode::LoadLocal:
  case Opcode::LoadGlobal:
    os << " @" << inst.imm;
    break;

  case Opcode::StoreLocal:
  case Opcode::StoreGlobal:
    os << " @" << inst.imm << ", ";
    printValue(os, inst.a);
    break;

  case Opcode::Alloca:
    os << ' ' << inst.imm;
    break;

  case Opcode::Call:
    os << " fn" << inst.funcIndex << '(';
    for (std::size_t i = 0; i < inst.args.size(); ++i) {
      if (i > 0) {
        os << ", ";
      }
      printValue(os, inst.args[i]);
    }
    os << ')';
    break;

  case Opcode::Jump:
    os << " b" << inst.targetTrue;
    break;

  case Opcode::Branch:
    os << ' ';
    printValue(os, inst.a);
    os << " ? b" << inst.targetTrue << " : b" << inst.targetFalse;
    break;

  case Opcode::Return:
    if (inst.a != kNoValue) {
      os << ' ';
      printValue(os, inst.a);
    }
    break;
  }

  os << '\n';
}

} // namespace

std::string printFunction(const Function& fn) {
  std::ostringstream os;

  os << "function " << typeName(fn.returnType) << ' ' << fn.name << '(' << fn.numParams
     << " params)  frame=" << fn.frameSize << " values=" << fn.nextValue << '\n';

  for (const BasicBlock& block : fn.blocks) {
    os << "  b" << block.id;
    if (!block.label.empty()) {
      os << " (" << block.label << ')';
    }
    if (!block.predecessors.empty()) {
      os << "  preds:";
      for (const BlockId p : block.predecessors) {
        os << " b" << p;
      }
    }
    os << '\n';

    for (const Instruction& inst : block.instructions) {
      printInstruction(os, inst);
    }
  }

  return os.str();
}

std::string printModule(const Module& module) {
  std::ostringstream os;

  for (const GlobalVarDecl& g : module.globals) {
    os << "global @" << g.index << ' ' << g.name;
    if (g.isArray) {
      os << '[' << g.arraySize << ']';
    } else {
      os << " = " << g.initValue;
    }
    os << '\n';
  }
  if (!module.globals.empty()) {
    os << '\n';
  }

  for (const Function& fn : module.functions) {
    os << printFunction(fn) << '\n';
  }

  return os.str();
}

} // namespace mloptc::ir
