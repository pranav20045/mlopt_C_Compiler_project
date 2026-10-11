#include "mloptc/pass_manager.hpp"

#include "mloptc/ir_verifier.hpp"

#include <sstream>

namespace mloptc::opt {

std::vector<PassPtr> createAllPasses() {
  std::vector<PassPtr> passes;
  passes.push_back(createLoadElimination());
  passes.push_back(createConstantFolding());
  passes.push_back(createCopyPropagation());
  passes.push_back(createCommonSubexpressionElimination());
  passes.push_back(createDeadCodeElimination());
  return passes;
}

std::vector<std::string> allPassNames() {
  std::vector<std::string> names;
  for (const PassPtr& pass : createAllPasses()) {
    names.emplace_back(pass->name());
  }
  return names;
}

PassPtr createPassByName(std::string_view name) {
  if (name == "loadelim") {
    return createLoadElimination();
  }
  if (name == "constfold") {
    return createConstantFolding();
  }
  if (name == "copyprop") {
    return createCopyPropagation();
  }
  if (name == "cse") {
    return createCommonSubexpressionElimination();
  }
  if (name == "dce") {
    return createDeadCodeElimination();
  }
  return nullptr;
}

Pipeline defaultO2Pipeline() {
  return Pipeline{"O2",
                  {"loadelim", "copyprop", "constfold", "cse", "copyprop", "dce",
                   "loadelim", "copyprop", "constfold", "cse", "copyprop", "dce"}};
}

std::string PipelineStats::report() const {
  std::ostringstream os;

  os << "pass statistics\n";
  os << "  instructions: " << instructionsBefore << " -> " << instructionsAfter;
  if (instructionsBefore > 0) {
    const double pct = 100.0 * static_cast<double>(instructionsBefore - instructionsAfter) /
                       static_cast<double>(instructionsBefore);
    os << "  (" << pct << "% removed)";
  }
  os << '\n';
  os << "  blocks:       " << blocksBefore << " -> " << blocksAfter << '\n';

  if (!perPass.empty()) {
    os << "\n  per pass:\n";
    for (const PassStats& s : perPass) {
      os << "    " << s.passName << ": removed=" << s.instructionsRemoved
         << " rewritten=" << s.instructionsRewritten << " blocksRemoved=" << s.blocksRemoved
         << '\n';
    }
  }

  return os.str();
}

namespace {

std::size_t countInstructions(const ir::Module& module) {
  std::size_t total = 0;
  for (const ir::Function& fn : module.functions) {
    total += fn.instructionCount();
  }
  return total;
}

std::size_t countBlocks(const ir::Module& module) {
  std::size_t total = 0;
  for (const ir::Function& fn : module.functions) {
    total += fn.blocks.size();
  }
  return total;
}

} // namespace

bool PassManager::run(ir::Module& module, const std::vector<std::string>& passNames,
                      PipelineStats& stats, std::string& error) {
  stats = PipelineStats{};
  stats.instructionsBefore = countInstructions(module);
  stats.blocksBefore = countBlocks(module);

  // Verify once before anything runs. If lowering produced broken IR, we want
  // to hear about it here rather than blaming the first pass.
  if (options_.verifyAfterEachPass) {
    const ir::VerificationResult before = ir::verify(module);
    if (!before.ok()) {
      error = "IR is invalid before optimization:\n" + before.message();
      return false;
    }
  }

  for (const std::string& passName : passNames) {
    PassPtr pass = createPassByName(passName);
    if (!pass) {
      error = "unknown pass '" + passName + "'";
      return false;
    }

    PassStats combined;
    combined.passName = passName;

    for (ir::Function& fn : module.functions) {
      const PassStats one = pass->run(fn, module);
      combined.instructionsRemoved += one.instructionsRemoved;
      combined.instructionsRewritten += one.instructionsRewritten;
      combined.blocksRemoved += one.blocksRemoved;

      // Verify per FUNCTION, not per module, so the error names the function
      // the pass actually broke.
      if (options_.verifyAfterEachPass) {
        const ir::VerificationResult result =
            ir::verify(fn, static_cast<std::uint32_t>(module.globals.size()));
        if (!result.ok()) {
          error = "pass '" + passName + "' produced invalid IR in function '" + fn.name +
                  "':\n" + result.message() + "\n--- IR after the pass ---\n" +
                  ir::printFunction(fn);
          if (options_.abortOnVerificationFailure) {
            return false;
          }
        }
      }
    }

    stats.perPass.push_back(std::move(combined));
  }

  stats.instructionsAfter = countInstructions(module);
  stats.blocksAfter = countBlocks(module);
  return true;
}

bool optimizeModule(ir::Module& module, const std::vector<std::string>& passNames,
                    PipelineStats& stats, std::string& error) {
  PassManager manager;
  return manager.run(module, passNames, stats, error);
}

} // namespace mloptc::opt
