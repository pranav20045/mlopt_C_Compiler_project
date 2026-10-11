#include "mloptc/driver.hpp"
#include "mloptc/interpreter.hpp"
#include "mloptc/ir_features.hpp"
#include "mloptc/ir_verifier.hpp"
#include "mloptc/llvm_emitter.hpp"
#include "mloptc/lowering.hpp"
#include "mloptc/ml_scheduler.hpp"
#include "mloptc/pass_manager.hpp"
#include "mloptc/version.hpp"
#include "mloptc/vm.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace mloptc;

void printUsage() {
  std::cout << "MLOpt-C -- an optimizing compiler for ToyC\n\n"
               "usage: mloptc [options] <file.tc>\n\n"
               "execution:\n"
               "  --run-ast          run on the AST interpreter (the reference oracle)\n"
               "  --run-vm           run on the IR virtual machine (default)\n"
               "  --emit-llvm        write LLVM IR to stdout (or to --output)\n\n"
               "optimization:\n"
               "  -O0                no optimization (default)\n"
               "  -O2                the fixed pipeline\n"
               "  -O-ml              let the trained model pick the pass order\n"
               "  --model FILE       model JSON for -O-ml (default ml/models/model.json)\n"
               "  --passes a,b,c     run exactly these passes, in this order\n"
               "  --list-passes      show every available pass and exit\n\n"
               "inspection:\n"
               "  --emit-ast         print the type-checked AST\n"
               "  --emit-ir          print the IR after optimization\n"
               "  --emit-features    print the IR feature vector as JSON\n"
               "  --stats            print pass statistics and the dynamic instruction count\n"
               "  --json             print results as JSON (used by the ML tooling)\n\n"
               "other:\n"
               "  -o, --output FILE  write --emit-llvm output to FILE\n"
               "  --version          print version and exit\n"
               "  -h, --help         print this message and exit\n";
}

void listPasses() {
  std::cout << "available passes:\n";
  for (const opt::PassPtr& pass : opt::createAllPasses()) {
    std::cout << "  " << pass->name() << "\n      " << pass->description() << '\n';
  }
  std::cout << "\nthe -O2 pipeline is:\n  ";
  const opt::Pipeline pipeline = opt::defaultO2Pipeline();
  for (const std::string& name : pipeline.passNames) {
    std::cout << name << ' ';
  }
  std::cout << '\n';
}

std::vector<std::string> splitCommaList(std::string_view text) {
  std::vector<std::string> parts;
  std::string current;
  for (const char c : text) {
    if (c == ',') {
      if (!current.empty()) {
        parts.push_back(current);
      }
      current.clear();
    } else if (c != ' ') {
      current += c;
    }
  }
  if (!current.empty()) {
    parts.push_back(current);
  }
  return parts;
}

/// Escapes a string for inclusion in JSON. Only the characters our output can
/// actually contain need handling: ToyC output is digits, minus signs and
/// newlines.
std::string jsonEscape(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (const char c : text) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      out += c;
      break;
    }
  }
  return out;
}

struct Options {
  std::string inputPath;
  std::string outputPath;

  bool runAst = false;
  bool runVm = false;
  bool emitLlvm = false;
  bool emitAst = false;
  bool emitIr = false;
  bool stats = false;
  bool json = false;
  bool emitFeatures = false;
  bool useMl = false;
  std::string modelPath = "ml/models/model.json";

  std::vector<std::string> passes;
};

} // namespace

int main(int argc, char** argv) {
  const std::vector<std::string_view> args(argv + 1, argv + argc);

  if (args.empty()) {
    printUsage();
    return 1;
  }

  Options options;

  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string_view arg = args[i];

    if (arg == "--version") {
      std::cout << mloptc::versionBanner() << '\n';
      return 0;
    }
    if (arg == "-h" || arg == "--help") {
      printUsage();
      return 0;
    }
    if (arg == "--list-passes") {
      listPasses();
      return 0;
    }

    if (arg == "--run-ast") {
      options.runAst = true;
    } else if (arg == "--run-vm") {
      options.runVm = true;
    } else if (arg == "--emit-llvm") {
      options.emitLlvm = true;
    } else if (arg == "--emit-ast") {
      options.emitAst = true;
    } else if (arg == "--emit-ir") {
      options.emitIr = true;
    } else if (arg == "--stats") {
      options.stats = true;
    } else if (arg == "--json") {
      options.json = true;
    } else if (arg == "-O0") {
      options.passes.clear();
    } else if (arg == "-O2") {
      options.passes = opt::defaultO2Pipeline().passNames;
    } else if (arg == "-O-ml") {
      options.useMl = true;
    } else if (arg == "--emit-features") {
      options.emitFeatures = true;
    } else if (arg == "--model") {
      if (i + 1 >= args.size()) {
        std::cerr << "mloptc: --model requires a file name" << '\n';
        return 2;
      }
      options.modelPath = std::string(args[++i]);
    } else if (arg == "--passes") {
      if (i + 1 >= args.size()) {
        std::cerr << "mloptc: --passes requires a comma-separated list\n";
        return 2;
      }
      options.passes = splitCommaList(args[++i]);
    } else if (arg == "-o" || arg == "--output") {
      if (i + 1 >= args.size()) {
        std::cerr << "mloptc: " << arg << " requires a file name\n";
        return 2;
      }
      options.outputPath = std::string(args[++i]);
    } else if (!arg.empty() && arg[0] == '-') {
      std::cerr << "mloptc: unknown option '" << arg << "'\n";
      return 2;
    } else {
      options.inputPath = std::string(arg);
    }
  }

  if (options.inputPath.empty()) {
    std::cerr << "mloptc: no input file\n";
    return 2;
  }

  // Running on the VM is the default when nothing else was asked for.
  if (!options.runAst && !options.emitLlvm && !options.emitAst && !options.emitIr &&
      !options.emitFeatures) {
    options.runVm = true;
  }

  const auto source = readFile(options.inputPath);
  if (!source) {
    std::cerr << "mloptc: cannot open '" << options.inputPath << "'\n";
    return 2;
  }

  DiagnosticEngine diags;
  auto program = compileFrontend(*source, diags);
  if (!program) {
    diags.print(std::cerr, options.inputPath);
    return 1;
  }
  diags.print(std::cerr, options.inputPath); // any warnings

  if (options.emitAst) {
    std::cout << dumpAst(*program);
  }

  // --- the AST path (the oracle) ---------------------------------------------
  // Deliberately not affected by -O2: it is the reference, so it must always
  // execute the program exactly as written.
  if (options.runAst) {
    const ExecutionResult result = interpret(*program);
    std::cout << result.output;
    if (!result.ok()) {
      std::cerr << runtimeStatusMessage(result.status) << '\n';
    }
    if (options.stats) {
      std::cerr << "ast nodes visited: " << result.dynamicInstructions << '\n';
    }
    return result.exitStatus;
  }

  // --- lower and optimize ----------------------------------------------------
  ir::Module module = lower(*program);

  // Features are read off the UNOPTIMIZED IR, which is the whole point: the
  // model has to predict which pass order will help from what the program looks
  // like BEFORE any pass has run.
  if (options.emitFeatures) {
    std::cout << ml::featuresToJson(ml::extractFeatures(module)) << '\n';
    if (!options.runVm && !options.emitIr && !options.emitLlvm) {
      return 0;
    }
  }

  if (options.useMl) {
    std::string modelError;
    const auto model = ml::Model::loadFromFile(options.modelPath, modelError);
    if (!model) {
      // Falling back to -O2 rather than failing: a missing model should not
      // stop the compiler from compiling. The warning says exactly what to do.
      std::cerr << "mloptc: -O-ml: " << modelError << '\n'
                << "mloptc: train one with 'python ml/train.py'; using -O2 instead" << '\n';
      options.passes = opt::defaultO2Pipeline().passNames;
    } else {
      options.passes = model->choosePipeline(module);
      if (options.stats) {
        std::cerr << "ml-selected pipeline:";
        for (const std::string& name : options.passes) {
          std::cerr << ' ' << name;
        }
        std::cerr << '\n';
      }
    }
  }

  opt::PipelineStats pipelineStats;
  std::string error;
  if (!opt::optimizeModule(module, options.passes, pipelineStats, error)) {
    std::cerr << "mloptc: " << error << '\n';
    return 3;
  }

  const ir::VerificationResult verification = ir::verify(module);
  if (!verification.ok()) {
    std::cerr << "mloptc: internal error, IR is invalid after optimization:\n"
              << verification.message();
    return 4;
  }

  if (options.emitIr) {
    std::cout << ir::printModule(module);
  }

  if (options.emitLlvm) {
    const std::string llvm = emitLlvmIr(module);
    if (options.outputPath.empty()) {
      std::cout << llvm;
    } else {
      std::ofstream out(options.outputPath, std::ios::binary);
      if (!out) {
        std::cerr << "mloptc: cannot write '" << options.outputPath << "'\n";
        return 2;
      }
      out << llvm;
    }
    return 0;
  }

  if (options.runVm) {
    const ExecutionResult result = execute(module);

    if (options.json) {
      // One line of JSON, so the Python tooling can parse it without caring
      // about anything else the compiler printed.
      std::cout << "{\"output\":\"" << jsonEscape(result.output)
                << "\",\"exit_status\":" << result.exitStatus
                << ",\"dynamic_instructions\":" << result.dynamicInstructions
                << ",\"static_instructions\":" << pipelineStats.instructionsAfter
                << ",\"static_instructions_before\":" << pipelineStats.instructionsBefore
                << ",\"blocks\":" << pipelineStats.blocksAfter << ",\"status\":\""
                << jsonEscape(runtimeStatusMessage(result.status)) << "\"}\n";
      return result.exitStatus;
    }

    std::cout << result.output;
    if (!result.ok()) {
      std::cerr << runtimeStatusMessage(result.status) << '\n';
    }
    if (options.stats) {
      std::cerr << pipelineStats.report()
                << "  dynamic instructions: " << result.dynamicInstructions << '\n';
    }
    return result.exitStatus;
  }

  if (options.stats) {
    std::cerr << pipelineStats.report();
  }

  return 0;
}
