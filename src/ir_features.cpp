#include "mloptc/ir_features.hpp"

#include <cmath>
#include <map>
#include <sstream>
#include <tuple>
#include <vector>

namespace mloptc::ml {

const std::array<std::string_view, kFeatureCount>& featureNames() noexcept {
  static const std::array<std::string_view, kFeatureCount> names = {
      "log_instruction_count",
      "log_block_count",
      "log_function_count",
      "avg_block_size",
      "max_block_size",
      "back_edge_ratio",
      "max_loop_depth_estimate",
      "branch_density",
      "avg_successors",
      "arithmetic_ratio",
      "comparison_ratio",
      "memory_ratio",
      "array_ratio",
      "call_ratio",
      "const_ratio",
      "move_ratio",
      "div_rem_ratio",
      "print_ratio",
      "redundancy_ratio",
      "forwardable_load_ratio",
      "dead_instruction_ratio",
      "const_operand_ratio",
  };
  return names;
}

namespace {

/// Guards every ratio. Returning 0 for an empty denominator keeps a tiny
/// program from producing NaN, which would silently poison a trained model --
/// most learners either crash or quietly treat NaN as a split point.
double safeRatio(double numerator, double denominator) {
  return denominator > 0.0 ? numerator / denominator : 0.0;
}

double logScale(std::size_t n) {
  return std::log1p(static_cast<double>(n));
}

struct Counters {
  std::size_t instructions = 0;
  std::size_t blocks = 0;
  std::size_t maxBlockSize = 0;

  std::size_t backEdges = 0;
  std::size_t conditionalBlocks = 0;
  std::size_t successorTotal = 0;

  std::size_t arithmetic = 0;
  std::size_t comparison = 0;
  std::size_t memory = 0;
  std::size_t array = 0;
  std::size_t calls = 0;
  std::size_t constants = 0;
  std::size_t moves = 0;
  std::size_t divRem = 0;
  std::size_t prints = 0;

  std::size_t pureInstructions = 0;
  std::size_t redundantPure = 0;

  std::size_t loads = 0;
  std::size_t forwardableLoads = 0;

  std::size_t valuesProduced = 0;
  std::size_t valuesUnused = 0;

  std::size_t binaryOps = 0;
  std::size_t binaryOpsWithConstOperand = 0;

  /// Deepest nesting of back edges containing a block, approximated by how many
  /// loop ranges [target, source] enclose it. Exact for the reducible,
  /// properly nested CFGs our lowering produces from structured control flow.
  std::size_t maxLoopDepth = 0;
};

void countInstruction(const ir::Instruction& inst, Counters& c) {
  using ir::Opcode;

  switch (inst.op) {
  case Opcode::Add:
  case Opcode::Sub:
  case Opcode::Mul:
  case Opcode::Neg:
    ++c.arithmetic;
    break;
  case Opcode::Div:
  case Opcode::Rem:
    ++c.divRem;
    break;
  case Opcode::Lt:
  case Opcode::Le:
  case Opcode::Gt:
  case Opcode::Ge:
  case Opcode::Eq:
  case Opcode::Ne:
  case Opcode::Not:
    ++c.comparison;
    break;
  case Opcode::LoadLocal:
  case Opcode::LoadGlobal:
  case Opcode::StoreLocal:
  case Opcode::StoreGlobal:
    ++c.memory;
    break;
  case Opcode::ArrayLoad:
  case Opcode::ArrayStore:
  case Opcode::Alloca:
    ++c.array;
    break;
  case Opcode::Call:
    ++c.calls;
    break;
  case Opcode::Const:
    ++c.constants;
    break;
  case Opcode::Move:
    ++c.moves;
    break;
  case Opcode::Print:
    ++c.prints;
    break;
  case Opcode::Jump:
  case Opcode::Branch:
  case Opcode::Return:
    break;
  }
}

/// Counts pure computations that repeat within a block -- what CSE could take.
void countRedundancy(const ir::BasicBlock& block, Counters& c) {
  std::map<std::tuple<ir::Opcode, ir::ValueId, ir::ValueId>, int> seen;

  for (const ir::Instruction& inst : block.instructions) {
    if (!ir::isPure(inst.op) || inst.dest == ir::kNoValue) {
      continue;
    }
    ++c.pureInstructions;

    ir::ValueId a = inst.a;
    ir::ValueId b = inst.b;
    if (ir::isCommutative(inst.op) && b != ir::kNoValue && b < a) {
      std::swap(a, b);
    }
    if (++seen[{inst.op, a, b}] > 1) {
      ++c.redundantPure;
    }
  }
}

/// Counts loads that a store earlier in the same block could satisfy -- what
/// loadelim could forward.
void countForwardableLoads(const ir::BasicBlock& block, Counters& c) {
  std::map<std::pair<bool, std::int32_t>, bool> known; // (isGlobal, slot) -> seen

  for (const ir::Instruction& inst : block.instructions) {
    switch (inst.op) {
    case ir::Opcode::StoreLocal:
      known[{false, inst.imm}] = true;
      break;
    case ir::Opcode::StoreGlobal:
      known[{true, inst.imm}] = true;
      break;
    case ir::Opcode::LoadLocal:
      ++c.loads;
      if (known[{false, inst.imm}]) {
        ++c.forwardableLoads;
      }
      known[{false, inst.imm}] = true;
      break;
    case ir::Opcode::LoadGlobal:
      ++c.loads;
      if (known[{true, inst.imm}]) {
        ++c.forwardableLoads;
      }
      known[{true, inst.imm}] = true;
      break;
    case ir::Opcode::Call:
      // Matches loadelim: a call may write any global, but cannot reach our
      // frame.
      for (auto& [key, value] : known) {
        if (key.first) {
          value = false;
        }
      }
      break;
    default:
      break;
    }
  }
}

void analyzeFunction(const ir::Function& fn, Counters& c) {
  c.blocks += fn.blocks.size();

  // Back edges, and nesting depth, from loop ranges.
  std::vector<std::pair<ir::BlockId, ir::BlockId>> loops;
  for (const ir::BasicBlock& block : fn.blocks) {
    c.successorTotal += block.successors.size();
    if (block.successors.size() > 1) {
      ++c.conditionalBlocks;
    }
    for (const ir::BlockId succ : block.successors) {
      if (succ <= block.id) {
        ++c.backEdges;
        loops.emplace_back(succ, block.id);
      }
    }
  }

  for (const ir::BasicBlock& block : fn.blocks) {
    std::size_t depth = 0;
    for (const auto& [head, tail] : loops) {
      if (block.id >= head && block.id <= tail) {
        ++depth;
      }
    }
    c.maxLoopDepth = std::max(c.maxLoopDepth, depth);
  }

  std::vector<std::uint32_t> useCount(fn.nextValue, 0);
  for (const ir::BasicBlock& block : fn.blocks) {
    for (const ir::Instruction& inst : block.instructions) {
      inst.forEachUse([&](ir::ValueId use) {
        if (use != ir::kNoValue && use < useCount.size()) {
          ++useCount[use];
        }
      });
    }
  }

  for (const ir::BasicBlock& block : fn.blocks) {
    c.instructions += block.instructions.size();
    c.maxBlockSize = std::max(c.maxBlockSize, block.instructions.size());

    for (const ir::Instruction& inst : block.instructions) {
      countInstruction(inst, c);

      if (inst.dest != ir::kNoValue && inst.dest < useCount.size()) {
        ++c.valuesProduced;
        if (useCount[inst.dest] == 0) {
          ++c.valuesUnused;
        }
      }

      if (inst.b != ir::kNoValue && inst.a != ir::kNoValue) {
        ++c.binaryOps;
      }
    }

    countRedundancy(block, c);
    countForwardableLoads(block, c);
  }

  // A binary op "has a constant operand" if either operand is defined by a
  // Const. Single assignment makes this a simple lookup rather than an
  // analysis.
  std::vector<bool> isConst(fn.nextValue, false);
  for (const ir::BasicBlock& block : fn.blocks) {
    for (const ir::Instruction& inst : block.instructions) {
      if (inst.op == ir::Opcode::Const && inst.dest != ir::kNoValue && inst.dest < isConst.size()) {
        isConst[inst.dest] = true;
      }
    }
  }
  for (const ir::BasicBlock& block : fn.blocks) {
    for (const ir::Instruction& inst : block.instructions) {
      if (inst.a == ir::kNoValue || inst.b == ir::kNoValue) {
        continue;
      }
      const bool aConst = inst.a < isConst.size() && isConst[inst.a];
      const bool bConst = inst.b < isConst.size() && isConst[inst.b];
      if (aConst || bConst) {
        ++c.binaryOpsWithConstOperand;
      }
    }
  }
}

} // namespace

FeatureVector extractFeatures(const ir::Module& module) {
  Counters c;
  for (const ir::Function& fn : module.functions) {
    analyzeFunction(fn, c);
  }

  FeatureVector f{};
  const auto set = [&f](Feature key, double value) { f[static_cast<std::size_t>(key)] = value; };

  const auto instructions = static_cast<double>(c.instructions);
  const auto blocks = static_cast<double>(c.blocks);

  set(Feature::LogInstructionCount, logScale(c.instructions));
  set(Feature::LogBlockCount, logScale(c.blocks));
  set(Feature::LogFunctionCount, logScale(module.functions.size()));

  set(Feature::AvgBlockSize, safeRatio(instructions, blocks));
  set(Feature::MaxBlockSize, static_cast<double>(c.maxBlockSize));

  set(Feature::BackEdgeRatio, safeRatio(static_cast<double>(c.backEdges), blocks));
  set(Feature::MaxLoopDepthEstimate, static_cast<double>(c.maxLoopDepth));

  set(Feature::BranchDensity, safeRatio(static_cast<double>(c.conditionalBlocks), blocks));
  set(Feature::AvgSuccessors, safeRatio(static_cast<double>(c.successorTotal), blocks));

  set(Feature::ArithmeticRatio, safeRatio(static_cast<double>(c.arithmetic), instructions));
  set(Feature::ComparisonRatio, safeRatio(static_cast<double>(c.comparison), instructions));
  set(Feature::MemoryRatio, safeRatio(static_cast<double>(c.memory), instructions));
  set(Feature::ArrayRatio, safeRatio(static_cast<double>(c.array), instructions));
  set(Feature::CallRatio, safeRatio(static_cast<double>(c.calls), instructions));
  set(Feature::ConstRatio, safeRatio(static_cast<double>(c.constants), instructions));
  set(Feature::MoveRatio, safeRatio(static_cast<double>(c.moves), instructions));
  set(Feature::DivRemRatio, safeRatio(static_cast<double>(c.divRem), instructions));
  set(Feature::PrintRatio, safeRatio(static_cast<double>(c.prints), instructions));

  set(Feature::RedundancyRatio,
      safeRatio(static_cast<double>(c.redundantPure), static_cast<double>(c.pureInstructions)));
  set(Feature::ForwardableLoadRatio,
      safeRatio(static_cast<double>(c.forwardableLoads), static_cast<double>(c.loads)));
  set(Feature::DeadInstructionRatio,
      safeRatio(static_cast<double>(c.valuesUnused), static_cast<double>(c.valuesProduced)));
  set(Feature::ConstOperandRatio, safeRatio(static_cast<double>(c.binaryOpsWithConstOperand),
                                            static_cast<double>(c.binaryOps)));

  return f;
}

std::string featuresToJson(const FeatureVector& features) {
  std::ostringstream os;
  os << '{';
  const auto& names = featureNames();
  for (std::size_t i = 0; i < kFeatureCount; ++i) {
    if (i > 0) {
      os << ',';
    }
    os << '"' << names[i] << "\":" << features[i];
  }
  os << '}';
  return os.str();
}

} // namespace mloptc::ml
