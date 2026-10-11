#include "mloptc/ir_verifier.hpp"

#include <algorithm>
#include <unordered_set>

namespace mloptc::ir {

std::string VerificationResult::message() const {
  std::string out;
  for (const std::string& e : errors_) {
    out += e;
    out += '\n';
  }
  return out;
}

namespace {

std::string blockRef(const Function& fn, BlockId id) {
  return fn.name + ":b" + std::to_string(id);
}

/// Reverse postorder of the CFG.
///
/// Why this order: in reverse postorder every block appears after at least one
/// of its predecessors (except the entry, and except via back edges). Walking
/// in that order lets a single linear scan check "is every register defined
/// before it is used" without building a dominator tree.
std::vector<BlockId> reversePostorder(const Function& fn) {
  std::vector<BlockId> order;
  if (fn.blocks.empty()) {
    return order;
  }

  std::vector<bool> visited(fn.blocks.size(), false);
  std::vector<std::pair<BlockId, std::size_t>> stack;
  stack.emplace_back(fn.entry, 0);
  visited[fn.entry] = true;

  while (!stack.empty()) {
    auto& [id, next] = stack.back();
    const BasicBlock& block = fn.blocks[id];

    if (next < block.successors.size()) {
      const BlockId succ = block.successors[next++];
      if (succ < fn.blocks.size() && !visited[succ]) {
        visited[succ] = true;
        stack.emplace_back(succ, 0);
      }
      continue;
    }

    order.push_back(id);
    stack.pop_back();
  }

  std::reverse(order.begin(), order.end());
  return order;
}

} // namespace

VerificationResult verify(const Function& fn, std::uint32_t globalCount) {
  VerificationResult result;

  if (fn.blocks.empty()) {
    result.addError(fn.name + ": function has no blocks");
    return result;
  }

  if (fn.entry >= fn.blocks.size()) {
    result.addError(fn.name + ": entry block " + std::to_string(fn.entry) + " does not exist");
    return result;
  }

  // --- block ids are consistent with their position ---
  for (BlockId i = 0; i < fn.blocks.size(); ++i) {
    if (fn.blocks[i].id != i) {
      result.addError(fn.name + ": block at index " + std::to_string(i) + " has id " +
                      std::to_string(fn.blocks[i].id));
    }
  }

  // --- terminators and branch targets ---
  for (const BasicBlock& block : fn.blocks) {
    if (block.instructions.empty()) {
      result.addError(blockRef(fn, block.id) + ": block is empty (needs a terminator)");
      continue;
    }

    if (!isTerminator(block.instructions.back().op)) {
      result.addError(blockRef(fn, block.id) + ": block does not end in a terminator (ends in '" +
                      std::string(opcodeName(block.instructions.back().op)) + "')");
    }

    for (std::size_t i = 0; i + 1 < block.instructions.size(); ++i) {
      if (isTerminator(block.instructions[i].op)) {
        result.addError(blockRef(fn, block.id) + ": terminator '" +
                        std::string(opcodeName(block.instructions[i].op)) + "' at position " +
                        std::to_string(i) + " is not the last instruction");
      }
    }

    for (const Instruction& inst : block.instructions) {
      const auto checkTarget = [&](BlockId target, const char* which) {
        if (target == kNoBlock || target >= fn.blocks.size()) {
          result.addError(blockRef(fn, block.id) + ": '" + std::string(opcodeName(inst.op)) +
                          "' has an invalid " + which + " target " + std::to_string(target));
        }
      };
      if (inst.op == Opcode::Jump) {
        checkTarget(inst.targetTrue, "jump");
      } else if (inst.op == Opcode::Branch) {
        checkTarget(inst.targetTrue, "true");
        checkTarget(inst.targetFalse, "false");
      }
    }
  }

  // --- each ValueId defined exactly once ---
  //
  // This is the property the optimization passes depend on. If a register were
  // assigned in two places, "the value of %5" would depend on which assignment
  // ran last, and constant folding or copy propagation could substitute the
  // wrong one. Checking it here means the passes may simply assume it.
  std::vector<int> defCount(fn.nextValue, 0);
  for (const BasicBlock& block : fn.blocks) {
    for (const Instruction& inst : block.instructions) {
      if (inst.dest == kNoValue) {
        continue;
      }
      if (inst.dest >= fn.nextValue) {
        result.addError(blockRef(fn, block.id) + ": instruction defines %" +
                        std::to_string(inst.dest) + ", beyond nextValue " +
                        std::to_string(fn.nextValue));
        continue;
      }
      if (++defCount[inst.dest] > 1) {
        result.addError(fn.name + ": %" + std::to_string(inst.dest) +
                        " is defined more than once");
      }
    }
  }

  // --- slot and global indices in range ---
  for (const BasicBlock& block : fn.blocks) {
    for (const Instruction& inst : block.instructions) {
      switch (inst.op) {
      case Opcode::LoadLocal:
      case Opcode::StoreLocal:
        if (inst.imm < 0 || static_cast<std::uint32_t>(inst.imm) >= fn.frameSize) {
          result.addError(blockRef(fn, block.id) + ": local slot " + std::to_string(inst.imm) +
                          " is outside the frame (size " + std::to_string(fn.frameSize) + ')');
        }
        break;
      case Opcode::LoadGlobal:
      case Opcode::StoreGlobal:
        if (inst.imm < 0 || static_cast<std::uint32_t>(inst.imm) >= globalCount) {
          result.addError(blockRef(fn, block.id) + ": global slot " + std::to_string(inst.imm) +
                          " is outside the module (count " + std::to_string(globalCount) + ')');
        }
        break;
      case Opcode::Alloca:
        if (inst.imm < 1) {
          result.addError(blockRef(fn, block.id) + ": alloca of " + std::to_string(inst.imm) +
                          " elements");
        }
        break;
      default:
        break;
      }
    }
  }

  // --- definition dominates use ---
  //
  // Walking in reverse postorder, a register must already have been defined by
  // the time we see it used. Parameters are an exception: they arrive in frame
  // slots, not registers, so nothing special is needed here.
  std::vector<bool> defined(fn.nextValue, false);
  for (const BlockId id : reversePostorder(fn)) {
    for (const Instruction& inst : fn.blocks[id].instructions) {
      inst.forEachUse([&](ValueId use) {
        if (use == kNoValue) {
          result.addError(blockRef(fn, id) + ": '" + std::string(opcodeName(inst.op)) +
                          "' uses an undefined operand");
          return;
        }
        if (use >= fn.nextValue) {
          result.addError(blockRef(fn, id) + ": use of %" + std::to_string(use) +
                          " beyond nextValue");
          return;
        }
        if (!defined[use]) {
          result.addError(blockRef(fn, id) + ": %" + std::to_string(use) +
                          " is used before it is defined");
        }
      });

      if (inst.dest != kNoValue && inst.dest < fn.nextValue) {
        defined[inst.dest] = true;
      }
    }
  }

  return result;
}

VerificationResult verify(const Module& module) {
  VerificationResult combined;
  const auto globalCount = static_cast<std::uint32_t>(module.globals.size());

  for (const Function& fn : module.functions) {
    const VerificationResult one = verify(fn, globalCount);
    for (const std::string& e : one.errors()) {
      combined.addError(e);
    }
  }

  if (module.mainIndex >= module.functions.size()) {
    combined.addError("module: mainIndex is out of range");
  }

  return combined;
}

} // namespace mloptc::ir
