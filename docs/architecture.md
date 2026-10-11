# Architecture

A tour of the codebase, in the order data flows through it. Each section names
the files, states what the stage guarantees to the next one, and points at the
one or two decisions that shape it.

---

## The pipeline

```
                          source.tc
                              |
   ┌──────────────────────────┴──────────────────────────┐
   │                      FRONT END                       │
   │  lexer.cpp ──► parser.cpp ──► sema.cpp               │
   │  tokens        AST            resolved, typed AST    │
   └──────────────────────────┬──────────────────────────┘
                              │
          ┌───────────────────┴───────────────────┐
          │                                       │
   interpreter.cpp                          lowering.cpp
   AST interpreter                          three-address IR + CFG
   ▸ REFERENCE ORACLE                              │
   ▸ never optimized                               │
                                     ┌─────────────┴─────────────┐
                                     │   ir_verifier.cpp         │
                                     │   runs after EVERY pass   │
                                     └─────────────┬─────────────┘
                                                   │
                                     ┌─────────────┴─────────────┐
                                     │   pass_manager.cpp        │
                                     │   passes/*.cpp            │
                                     │   loadelim constfold      │
                                     │   copyprop cse dce        │
                                     └─────────────┬─────────────┘
                                                   │
                          ┌────────────────────────┴───────────────┐
                          │                                        │
                   vm.cpp                                 llvm_emitter.cpp
                   IR virtual machine                     LLVM IR text
                   ▸ counts instructions                         │
                   ▸ the ML cost signal                    clang + runtime/
                                                                 │
                                                           native binary
```

Three execution paths. The differential tester (`verify/`) runs all of them and
compares. Because ToyC has no undefined behaviour, any disagreement is a bug.

---

## Front end

| File | Responsibility |
|---|---|
| `src/lexer.cpp` | source text → tokens |
| `src/parser.cpp` | tokens → AST |
| `src/sema.cpp` | name resolution, type checking, slot assignment |
| `src/diagnostics.cpp` | error collection and reporting |

**Guarantees to the next stage.** The lexer always produces exactly one trailing
`EndOfFile` token and never fails — bad input becomes `TokenKind::Unknown` with
a diagnostic, so the parser still sees a well-formed stream. After `sema`, every
name is resolved to a `(scope, slot)` pair and every call to a function index,
so no later stage ever looks a name up again.

**Shaping decision.** Each function gets one flat frame. Shadowed and
out-of-scope locals each get their own slot, so lexical scoping is resolved
entirely at compile time and nothing about scopes survives into execution. The
backends therefore never need a scope chain.

---

## The oracle

| File | Responsibility |
|---|---|
| `src/interpreter.cpp` | executes the AST directly |

Written to be *obviously correct* rather than fast. Every rule in
[`spec.md`](spec.md) maps to a visible piece of code. It is never optimized,
because it is the reference everything else is measured against.

It shares `execution.hpp` with the other backends — and only that. Two
implementations that shared their logic would agree with each other even when
both were wrong.

---

## IR

| File | Responsibility |
|---|---|
| `include/mloptc/ir.hpp` | instruction set, blocks, CFG |
| `src/lowering.cpp` | AST → IR |
| `src/ir_verifier.cpp` | structural validation |

Three-address code: every instruction reads from registers and writes at most
one register, with nothing implicit. **Not SSA** — locals live in memory slots.

**The one property the passes rely on**: every `ValueId` is assigned exactly
once, enforced by the verifier. That makes "the value in `%r`" a
position-independent fact, so constant folding and copy propagation are valid
function-wide with no dataflow analysis.

**Where control flow goes.** `if`, `while`, `for`, `break`, `continue` and
short-circuit `&&`/`||` all become blocks and branches. After lowering, the
compiler no longer knows what a loop is — only that some blocks branch
backwards.

`for` gets a **separate update block** so `continue` can target it. Without
that, `continue` would skip the update and turn a counting loop into an infinite
one.

Short-circuit operators allocate a **frame slot** to hold the result across the
merge of two paths — the non-SSA equivalent of a phi node, and a concrete
illustration of what mem2reg exists to clean up.

---

## Optimizer

| Pass | What it does | Key constraint |
|---|---|---|
| `loadelim` | forwards stored values to later loads, within a block | a call invalidates globals, never locals |
| `constfold` | evaluates constant operations, folds constant branches | must not fold `1/0` or `INT_MIN/-1` |
| `copyprop` | replaces uses of a copy with the original | valid function-wide only because of single assignment |
| `cse` | reuses a value already computed in the same block | block-local: no dominator tree exists |
| `dce` | removes unused, non-trapping instructions | `Div`, `Rem`, `ArrayLoad` count as effectful |

`loadelim` is the one that matters most. In a non-SSA IR every variable read is
a fresh register, so without it the other four passes are nearly blind — see
[`DESIGN.md`](DESIGN.md) §6.

The pass manager runs the IR verifier after every pass in debug builds, so a
pass that corrupts the IR is named on the spot rather than blamed on the next
one.

---

## Backends

| File | Responsibility |
|---|---|
| `src/vm.cpp` | executes IR, counts dynamic instructions |
| `src/llvm_emitter.cpp` | IR → LLVM IR text |
| `runtime/mloptc_runtime.c` | print, checked division, bounds checks, array arena |

The VM's instruction count is the cost signal the ML layer optimizes: exactly
reproducible, machine-independent, and a proxy rather than a measurement.

The LLVM backend emits **text** and hands it to clang. The two details that
matter: no `nsw`/`nuw` anywhere (ToyC arithmetic wraps, and `nsw` would let
clang assume otherwise), and division and array access go through checked
runtime helpers rather than raw `sdiv`/`getelementptr`, which are undefined in
exactly the cases ToyC defines as traps.

---

## Verification

| File | Responsibility |
|---|---|
| `verify/toolchain.py` | drives all three execution paths |
| `verify/generator.py` | random well-formed ToyC programs |
| `verify/differential.py` | runs campaigns, reports mismatches |
| `verify/reducer.py` | shrinks a failing program to its essence |
| `verify/test_verification.py` | pytest suite, including golden tests |

The generator deliberately produces division by zero, out-of-bounds indices and
overflow: all three are *defined* in ToyC, so all three backends must agree on
them. That is the payoff for specifying away undefined behaviour.

The native path compiles with `clang -O2` even when mloptc applied no passes —
a backend emitting IR that relies on UB behaves correctly until an optimizer
acts on the assumption.

---

## ML layer

| File | Responsibility |
|---|---|
| `src/ir_features.cpp` | 22 static IR features |
| `src/ml_scheduler.cpp` | model loading and inference |
| `src/json.cpp` | a minimal strict JSON parser |
| `ml/dataset.py` | measures (program, sequence) pairs |
| `ml/train.py` | trains, evaluates, exports |

Inference runs in C++, so `-O-ml` is a real compiler flag rather than a Python
script driving the compiler from outside. The model refuses to load if its
feature list does not match the build — a mismatched model would still predict,
from the wrong numbers.

Selection is not plain argmin: among candidates the model cannot tell apart
(within its own held-out error), the compiler takes the **shortest**. See the
README for why that turned out to be the objective worth optimizing.
