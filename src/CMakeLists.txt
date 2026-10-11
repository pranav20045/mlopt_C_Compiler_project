# mloptc_core holds ALL real compiler logic. The executable in tools/ is a thin
# CLI shim. This split is what lets the test binary link against the exact same
# code the CLI runs -- without it, nothing in the compiler would be unit-testable.
add_library(mloptc_core STATIC
  ast.cpp
  diagnostics.cpp
  driver.cpp
  interpreter.cpp
  ir.cpp
  ir_features.cpp
  ir_verifier.cpp
  json.cpp
  ml_scheduler.cpp
  llvm_emitter.cpp
  lowering.cpp
  pass_manager.cpp
  passes/constant_folding.cpp
  passes/copy_propagation.cpp
  passes/cse.cpp
  passes/dead_code_elimination.cpp
  passes/load_elimination.cpp
  vm.cpp
  sema.cpp
  parser.cpp
  lexer.cpp
  token.cpp
  version.cpp
)

target_include_directories(mloptc_core
  PUBLIC  ${CMAKE_SOURCE_DIR}/include   # the library's contract: mloptc/*.hpp
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})  # internal headers, not part of the API

target_link_libraries(mloptc_core
  PRIVATE project_warnings
  PUBLIC  project_sanitizers)           # PUBLIC: instrumentation must match
                                        # across every target that links us
