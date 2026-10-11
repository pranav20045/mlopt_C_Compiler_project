# MLOpt-C -- Design Notes

A running log of design decisions and *why* they were made. Newest phase last.

---

## Project goal

A small optimizing compiler for **ToyC** (a C-like language) with two
independently verifiable execution paths and an ML-guided optimization pass
scheduler. The emphasis is **verification**: every optimization must provably
preserve program semantics, checked by differential testing against a reference
interpreter.

Pipeline:

```
source.tc
   |-- Lexer -> Parser -> AST -> Type checker
   |                              |
   |                              |-- AST interpreter  ...... REFERENCE ORACLE
   |                              |
   |                              '-- Lowering to three-address IR (+CFG)
   |                                         |
   |                                         |-- IR verifier (after every pass)
   |                                         |-- Pass manager -> optimizations
   |                                         |
   |                                         |-- IR VM (dynamic instr. count)
   |                                         '-- LLVM IR text emitter -> clang
```

Correctness claim: **oracle == VM == LLVM-native**, on hand-written tests and on
thousands of randomly generated programs.

---

## Phase A -- Foundation

### C++20, extensions off
`CMAKE_CXX_EXTENSIONS OFF` gives `-std=c++20` rather than `-std=gnu++20`, so we
cannot accidentally depend on a GNU-only extension that breaks the Windows
build. C++20 (over 17) buys us `std::span`, designated initialisers, and
`<concepts>`, all of which keep the IR code readable.

### Library + thin executable split
All compiler logic lives in the static library `mloptc_core`; `tools/main.cpp`
is a CLI shim with zero compiler logic. Rationale: a test binary cannot link
against an executable. Without this split, nothing in the compiler would be
unit-testable -- and Phase B's "AST interpreter as reference oracle" depends
entirely on being able to call into the compiler from test code.

### Warnings via an INTERFACE target, not global flags
`project_warnings` is an INTERFACE library that only *our* targets link against.
Setting `CMAKE_CXX_FLAGS` globally would also apply `-Werror` to GoogleTest's
sources, so a single warning in a dependency we do not control would break the
build. This is the most common CMake mistake in `-Werror` projects.

`-Wswitch` is the highest-value warning here: when a new IR opcode is added,
every `switch` that forgot to handle it becomes a build error. It is effectively
a type checker for our own compiler.

### Sanitizers: Linux CI is authoritative
ASan works with clang on Windows, but **UBSan's runtime support on Windows is
incomplete**. Rather than pretend otherwise, the GitHub Actions Ubuntu runner
runs the full `-fsanitize=address,undefined` build, and Windows stays the fast
local iteration loop. `-fno-sanitize-recover=all` makes UB abort on first
occurrence instead of printing a warning and continuing.

### Toolchain on Windows
clang on Windows defaults to `--target=x86_64-pc-windows-msvc`, which requires
the MSVC standard library and the Windows SDK import libraries. LLVM alone is
*not* a complete toolchain on Windows -- see `docs/PROGRESS.md` for the resolution.

### GoogleTest via FetchContent
Downloaded and built at configure time. No submodule, no system package, no
vendored source. Keeps the repo source-only, matching the "no heavy
dependencies" constraint.

---

## Phase B -- Frontend

_(not started)_

---

## Open questions / flagged decisions

- **Defined behaviour over UB.** Division by zero and out-of-bounds array access
  will be *defined* in the ToyC spec as a trap with a diagnostic, not undefined
  behaviour. This is what makes differential testing tractable: a random program
  generator cannot accidentally produce a program on which the three backends
  may legitimately disagree. Flagged because it diverges from C semantics.
- **No SSA form.** Locals live in memory slots (load/store), so no dominator
  tree, no phi nodes, no mem2reg. This removes the single largest time sink and
  leaves more work for the optimizer to do, which widens the measurable gap the
  ML scheduler is trying to exploit. Dominators + mem2reg are a stretch goal.
