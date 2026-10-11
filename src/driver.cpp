#include "mloptc/driver.hpp"

#include "mloptc/lexer.hpp"
#include "mloptc/parser.hpp"
#include "mloptc/sema.hpp"

#include <fstream>
#include <sstream>

namespace mloptc {

std::optional<Program> compileFrontend(std::string_view source, DiagnosticEngine& diags) {
  Lexer lexer(source, diags);
  std::vector<Token> tokens = lexer.tokenize();

  Parser parser(std::move(tokens), diags);
  Program program = parser.parseProgram();

  // Semantic analysis is skipped if parsing already failed: name resolution on
  // a broken AST produces a flood of follow-on errors that bury the real one.
  if (diags.hasErrors()) {
    return std::nullopt;
  }

  if (!analyze(program, diags)) {
    return std::nullopt;
  }

  return program;
}

std::optional<std::string> readFile(const std::string& path) {
  // Binary mode on purpose: on Windows, text mode would silently turn "\r\n"
  // into "\n" and change the byte offsets the lexer reports. The lexer treats
  // '\r' as whitespace, so handling it there keeps behaviour identical on
  // every platform.
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return std::nullopt;
  }

  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

} // namespace mloptc
