#include "mloptc/version.hpp"

#include <iostream>
#include <string_view>
#include <vector>

namespace {

void printUsage() {
  std::cout << "MLOpt-C -- an optimizing compiler for ToyC\n\n"
               "usage: mloptc [options] <file.tc>\n\n"
               "options:\n"
               "  --version      print version and exit\n"
               "  -h, --help     print this message and exit\n";
}

} // namespace

int main(int argc, char** argv) {
  const std::vector<std::string_view> args(argv + 1, argv + argc);

  if (args.empty()) {
    printUsage();
    return 1;
  }

  for (const std::string_view arg : args) {
    if (arg == "--version") {
      std::cout << mloptc::versionBanner() << '\n';
      return 0;
    }
    if (arg == "-h" || arg == "--help") {
      printUsage();
      return 0;
    }
  }

  std::cerr << "mloptc: no input handling yet (frontend lands on day 1-2)\n";
  return 1;
}
