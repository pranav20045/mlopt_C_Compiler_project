# MLOpt-C

An optimizing compiler for **ToyC**, a small C-like language, written in C++20 —
with a reference interpreter, a three-address IR and optimizer, two backends,
and an ML-guided optimization pass scheduler.

> **Status: in development.** Results sections below are placeholders until real
> measurements exist. No numbers in this README are invented.

## Why this project

The emphasis is **compiler verification**. Every optimization pass must preserve
program semantics, and that claim is tested rather than asserted: an AST
interpreter acts as a reference oracle, and a differential tester checks it
against the IR virtual machine and against LLVM-compiled native code on both
hand-written and randomly generated programs.

## Build

```
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Binaries land in `build/debug/bin/`.

### Requirements
- A C++20 compiler
- CMake ≥ 3.20, Ninja
- Python 3.10+ (for the differential tester and ML tooling)

## Architecture

```
source.tc
   ├─ Lexer → Parser → AST → Type checker
   │                           │
   │                           ├─ AST interpreter ........ REFERENCE ORACLE
   │                           │
   │                           └─ Lowering to three-address IR (+ CFG)
   │                                      │
   │                                      ├─ IR verifier (runs after every pass)
   │                                      ├─ Pass manager → optimizations
   │                                      │
   │                                      ├─ IR VM (dynamic instruction count)
   │                                      └─ LLVM IR text emitter → clang → native
```

Optimization passes: constant folding, copy propagation, dead code elimination,
local common subexpression elimination. Each reports statistics and is
independently toggleable from the CLI.

## Verification

_(placeholder — to be filled with real counts)_

| Check | Result |
|---|---|
| Hand-written test programs | — |
| Randomly generated programs, oracle vs VM | — |
| Randomly generated programs, oracle vs LLVM-native | — |

## ML pass ordering

_(placeholder — to be filled with measured results, including cases where the
learned scheduler does **not** beat the fixed pipeline)_

## Documentation

- [`docs/spec.md`](docs/spec.md) — the ToyC language specification
- [`docs/DESIGN.md`](docs/DESIGN.md) — design decisions and rationale
- [`docs/PROGRESS.md`](docs/PROGRESS.md) — development log

## License

MIT
