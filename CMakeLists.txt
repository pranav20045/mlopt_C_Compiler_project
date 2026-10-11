cmake_minimum_required(VERSION 3.20)

project(mlopt_c
  VERSION 0.1.0
  DESCRIPTION "MLOpt-C: an optimizing compiler for ToyC with ML-guided pass ordering"
  LANGUAGES CXX)

# --- Language level -----------------------------------------------------------
# EXTENSIONS OFF => we get -std=c++20, not -std=gnu++20. Keeps us portable and
# stops us accidentally depending on a GNU-only extension.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Emitted into the build dir; clangd / VS Code read it for go-to-definition.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Put all binaries in one predictable place so tests and Python tooling can
# find the compiler without guessing at generator-specific subdirectories.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# --- Options ------------------------------------------------------------------
option(MLOPTC_BUILD_TESTS      "Build the unit test suite"          ON)
option(MLOPTC_ENABLE_SANITIZERS "Build with AddressSanitizer+UBSan" OFF)
option(MLOPTC_WARNINGS_AS_ERRORS "Treat warnings as errors"         ON)

include(cmake/CompilerWarnings.cmake)
include(cmake/Sanitizers.cmake)
include(cmake/StackSize.cmake)

add_subdirectory(src)     # mloptc_core  (the compiler, as a library)
add_subdirectory(tools)   # mloptc       (the CLI executable)

if(MLOPTC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

message(STATUS "")
message(STATUS "MLOpt-C ${PROJECT_VERSION}")
message(STATUS "  compiler   : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "  tests      : ${MLOPTC_BUILD_TESTS}")
message(STATUS "  sanitizers : ${MLOPTC_ENABLE_SANITIZERS}")
message(STATUS "")
