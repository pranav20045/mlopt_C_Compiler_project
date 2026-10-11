#pragma once

#include "mloptc/ir.hpp"
#include "mloptc/pass.hpp"

#include <string>
#include <vector>

namespace mloptc::opt {

/// A named sequence of passes.
struct Pipeline {
  std::string name;
  std::vector<std::string> passNames;
};

/// The fixed pipeline the ML scheduler is measured against.
///
/// The order is not arbitrary, and the reasoning is the same reasoning the ML
/// model has to rediscover from data:
///
///   loadelim -> copyprop -> constfold -> cse -> copyprop -> dce   (twice)
///
///   1. loadelim FIRST, and it is the one that matters most. Without it every
///      variable read is a separate register, so constant folding, CSE and
///      copy propagation are all effectively blind. This is the block-local
///      stand-in for mem2reg.
///   2. copyprop BEFORE cse. loadelim leaves `move` instructions behind, so
///      two identical expressions still have different operand registers until
///      the copies are forwarded. Running cse first finds nothing -- this was
///      a real bug, caught by an idempotence test: a second run of the
///      pipeline kept finding redundancy the first had missed.
///   3. constfold, now that variables resolve to the registers holding them.
///   4. cse turns surviving redundant computations into copies.
///   5. copyprop forwards those, dce removes the corpses.
///   6. The whole sequence runs twice, because dce's branch pruning exposes
///      new constants and new straight-line code for loadelim to work on.
///
/// Running passes more than once is normal in real compilers for exactly this
/// reason: passes expose work for each other, so one sequential sweep leaves
/// value on the table.
[[nodiscard]] Pipeline defaultO2Pipeline();

/// What a whole pipeline run did.
struct PipelineStats {
  std::vector<PassStats> perPass;

  std::size_t instructionsBefore = 0;
  std::size_t instructionsAfter = 0;
  std::size_t blocksBefore = 0;
  std::size_t blocksAfter = 0;

  [[nodiscard]] std::string report() const;
};

struct PassManagerOptions {
  /// Verify the IR after every pass. Defaults to on in debug builds.
  ///
  /// This is the single most valuable debugging feature in the project: when a
  /// pass corrupts the IR, the failure is reported with the name of the pass
  /// that did it, rather than surfacing later as a wrong answer or a crash in
  /// an unrelated pass.
  bool verifyAfterEachPass =
#ifdef NDEBUG
      false
#else
      true
#endif
      ;

  /// If verification fails, abort with a diagnostic rather than continuing.
  bool abortOnVerificationFailure = true;
};

/// Runs a sequence of passes over every function in a module.
class PassManager {
public:
  explicit PassManager(PassManagerOptions options = {}) : options_(options) {}

  /// Runs the named passes in order. Unknown names are reported in `error` and
  /// cause the run to fail.
  [[nodiscard]] bool run(ir::Module& module, const std::vector<std::string>& passNames,
                         PipelineStats& stats, std::string& error);

  [[nodiscard]] bool run(ir::Module& module, const Pipeline& pipeline, PipelineStats& stats,
                         std::string& error) {
    return run(module, pipeline.passNames, stats, error);
  }

private:
  PassManagerOptions options_;
};

/// Convenience wrapper: lower, optimize, and report. Returns false on error.
[[nodiscard]] bool optimizeModule(ir::Module& module, const std::vector<std::string>& passNames,
                                  PipelineStats& stats, std::string& error);

} // namespace mloptc::opt
