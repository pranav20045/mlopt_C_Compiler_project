include(FetchContent)

# FetchContent downloads and builds GoogleTest as part of our configure step.
# No submodule, no system package, no vendored copy in the repo.
FetchContent_Declare(googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.15.2
  GIT_SHALLOW    TRUE)

# Make GoogleTest use the same CRT as us on Windows (otherwise link errors).
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(mloptc_tests
  unit/test_frontend.cpp
  unit/test_passes.cpp
  unit/test_vm.cpp
  unit/test_interpreter.cpp
  unit/test_lexer.cpp
  unit/test_version.cpp
)

target_link_libraries(mloptc_tests
  PRIVATE mloptc_core GTest::gtest_main project_warnings)

# Registers each TEST() with CTest individually, so `ctest` reports them
# separately rather than as one opaque pass/fail.
mloptc_set_stack_size(mloptc_tests)

include(GoogleTest)
gtest_discover_tests(mloptc_tests)
