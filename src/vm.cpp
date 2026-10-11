#include "mloptc/vm.hpp"

#include <string>
#include <vector>

namespace mloptc {

namespace {

using namespace ir;

class Vm {
public:
  Vm(const Module& module, const VmOptions& options) : module_(module), options_(options) {}

  ExecutionResult run() {
    initGlobals();

    std::int32_t exitValue = 0;
    if (callFunction(module_.functions[module_.mainIndex], {}, exitValue)) {
      result_.exitStatus = static_cast<int>(((exitValue % 256) + 256) % 256);
    } else {
      result_.exitStatus = status_ == RuntimeStatus::StepLimitExceeded ? kStepLimitExitStatus
                                                                       : kRuntimeErrorExitStatus;
    }

    result_.status = status_;
    result_.dynamicInstructions = instructions_;
    return std::move(result_);
  }

private:
  void initGlobals() {
    globals_.assign(module_.globals.size(), 0);
    for (const GlobalVarDecl& g : module_.globals) {
      globals_[g.index] = g.isArray ? allocArray(g.arraySize) : g.initValue;
    }
  }

  std::int32_t allocArray(std::int32_t size) {
    arrays_.emplace_back(static_cast<std::size_t>(size), 0);
    return static_cast<std::int32_t>(arrays_.size() - 1);
  }

  bool trap(RuntimeStatus s) {
    if (status_ == RuntimeStatus::Ok) {
      status_ = s;
    }
    return false;
  }

  /// Returns false when a trap or the instruction budget stops execution.
  bool callFunction(const Function& fn, const std::vector<std::int32_t>& args,
                    std::int32_t& returnValue) {
    if (depth_ >= kMaxCallDepth) {
      return trap(RuntimeStatus::CallDepthExceeded);
    }

    const std::size_t arenaMark = arrays_.size();
    const std::size_t frameMark = frame_.size();
    const std::size_t valuesMark = values_.size();

    frame_.resize(frameMark + fn.frameSize, 0); // spec 7.4: zeroed
    for (std::size_t i = 0; i < args.size(); ++i) {
      frame_[frameMark + i] = args[i];
    }

    // One flat register file per frame. Registers are numbered densely from 0
    // within each function, so a vector sized nextValue is exactly right.
    values_.resize(valuesMark + fn.nextValue, 0);

    ++depth_;
    const std::size_t savedFrameBase = frameBase_;
    const std::size_t savedValueBase = valueBase_;
    frameBase_ = frameMark;
    valueBase_ = valuesMark;

    returnValue = 0; // spec 5.4
    const bool ok = runBlocks(fn, returnValue);

    frameBase_ = savedFrameBase;
    valueBase_ = savedValueBase;
    --depth_;
    values_.resize(valuesMark);
    frame_.resize(frameMark);
    arrays_.resize(arenaMark);

    return ok;
  }

  bool runBlocks(const Function& fn, std::int32_t& returnValue) {
    BlockId current = fn.entry;

    for (;;) {
      const BasicBlock& block = fn.blocks[current];
      BlockId next = kNoBlock;

      for (const Instruction& inst : block.instructions) {
        if (++instructions_ > options_.maxInstructions) {
          return trap(RuntimeStatus::StepLimitExceeded);
        }

        if (!step(inst, returnValue, next)) {
          return false;
        }

        if (next != kNoBlock) {
          break; // a terminator chose the next block
        }
        if (returned_) {
          returned_ = false;
          return true;
        }
      }

      if (next == kNoBlock) {
        // Only reachable if the verifier was skipped and a block has no
        // terminator. Treating it as a return keeps the VM from running off
        // the end of the block list.
        return true;
      }
      current = next;
    }
  }

  std::int32_t& reg(ValueId id) { return values_[valueBase_ + id]; }

  std::int32_t regValue(ValueId id) const { return values_[valueBase_ + id]; }

  bool step(const Instruction& inst, std::int32_t& returnValue, BlockId& next) {
    switch (inst.op) {
    case Opcode::Const:
      reg(inst.dest) = inst.imm;
      return true;

    case Opcode::Move:
      reg(inst.dest) = regValue(inst.a);
      return true;

    case Opcode::Add:
      reg(inst.dest) = wrapAdd(regValue(inst.a), regValue(inst.b));
      return true;
    case Opcode::Sub:
      reg(inst.dest) = wrapSub(regValue(inst.a), regValue(inst.b));
      return true;
    case Opcode::Mul:
      reg(inst.dest) = wrapMul(regValue(inst.a), regValue(inst.b));
      return true;

    case Opcode::Div: {
      std::int32_t out = 0;
      RuntimeStatus s = RuntimeStatus::Ok;
      if (!checkedDiv(regValue(inst.a), regValue(inst.b), out, s)) {
        return trap(s);
      }
      reg(inst.dest) = out;
      return true;
    }
    case Opcode::Rem: {
      std::int32_t out = 0;
      RuntimeStatus s = RuntimeStatus::Ok;
      if (!checkedRem(regValue(inst.a), regValue(inst.b), out, s)) {
        return trap(s);
      }
      reg(inst.dest) = out;
      return true;
    }

    case Opcode::Lt:
      reg(inst.dest) = regValue(inst.a) < regValue(inst.b) ? 1 : 0;
      return true;
    case Opcode::Le:
      reg(inst.dest) = regValue(inst.a) <= regValue(inst.b) ? 1 : 0;
      return true;
    case Opcode::Gt:
      reg(inst.dest) = regValue(inst.a) > regValue(inst.b) ? 1 : 0;
      return true;
    case Opcode::Ge:
      reg(inst.dest) = regValue(inst.a) >= regValue(inst.b) ? 1 : 0;
      return true;
    case Opcode::Eq:
      reg(inst.dest) = regValue(inst.a) == regValue(inst.b) ? 1 : 0;
      return true;
    case Opcode::Ne:
      reg(inst.dest) = regValue(inst.a) != regValue(inst.b) ? 1 : 0;
      return true;

    case Opcode::Neg:
      reg(inst.dest) = wrapNeg(regValue(inst.a));
      return true;
    case Opcode::Not:
      reg(inst.dest) = regValue(inst.a) == 0 ? 1 : 0;
      return true;

    case Opcode::LoadLocal:
      reg(inst.dest) = frame_[frameBase_ + static_cast<std::size_t>(inst.imm)];
      return true;
    case Opcode::StoreLocal:
      frame_[frameBase_ + static_cast<std::size_t>(inst.imm)] = regValue(inst.a);
      return true;
    case Opcode::LoadGlobal:
      reg(inst.dest) = globals_[static_cast<std::size_t>(inst.imm)];
      return true;
    case Opcode::StoreGlobal:
      globals_[static_cast<std::size_t>(inst.imm)] = regValue(inst.a);
      return true;

    case Opcode::Alloca:
      reg(inst.dest) = allocArray(inst.imm);
      return true;

    case Opcode::ArrayLoad: {
      std::vector<std::int32_t>& array = arrays_[static_cast<std::size_t>(regValue(inst.a))];
      const std::int32_t index = regValue(inst.b);
      if (index < 0 || static_cast<std::size_t>(index) >= array.size()) {
        return trap(RuntimeStatus::IndexOutOfBounds);
      }
      reg(inst.dest) = array[static_cast<std::size_t>(index)];
      return true;
    }

    case Opcode::ArrayStore: {
      std::vector<std::int32_t>& array = arrays_[static_cast<std::size_t>(regValue(inst.a))];
      const std::int32_t index = regValue(inst.b);
      if (index < 0 || static_cast<std::size_t>(index) >= array.size()) {
        return trap(RuntimeStatus::IndexOutOfBounds);
      }
      array[static_cast<std::size_t>(index)] = regValue(inst.c);
      return true;
    }

    case Opcode::Call: {
      std::vector<std::int32_t> args;
      args.reserve(inst.args.size());
      for (const ValueId arg : inst.args) {
        args.push_back(regValue(arg));
      }

      std::int32_t returned = 0;
      // The callee reallocates values_ and frame_, which would invalidate any
      // reference we held into them -- so the result is captured by value and
      // written back only after the call returns.
      const std::uint32_t callee = inst.funcIndex;
      const ValueId dest = inst.dest;
      if (!callFunction(module_.functions[callee], args, returned)) {
        return false;
      }
      if (dest != kNoValue) {
        reg(dest) = returned;
      }
      return true;
    }

    case Opcode::Print:
      result_.output += std::to_string(regValue(inst.a));
      result_.output += '\n';
      return true;

    case Opcode::Jump:
      next = inst.targetTrue;
      return true;

    case Opcode::Branch:
      next = regValue(inst.a) != 0 ? inst.targetTrue : inst.targetFalse;
      return true;

    case Opcode::Return:
      if (inst.a != kNoValue) {
        returnValue = regValue(inst.a);
      }
      returned_ = true;
      return true;
    }
    return true;
  }

  const Module& module_;
  VmOptions options_;

  ExecutionResult result_;
  RuntimeStatus status_ = RuntimeStatus::Ok;

  std::vector<std::int32_t> globals_;
  std::vector<std::int32_t> frame_;  ///< one contiguous stack of frames
  std::vector<std::int32_t> values_; ///< one contiguous stack of register files
  std::vector<std::vector<std::int32_t>> arrays_;

  std::size_t frameBase_ = 0;
  std::size_t valueBase_ = 0;
  std::uint32_t depth_ = 0;
  std::uint64_t instructions_ = 0;
  bool returned_ = false;
};

} // namespace

ExecutionResult execute(const ir::Module& module, const VmOptions& options) {
  Vm vm(module, options);
  return vm.run();
}

} // namespace mloptc
