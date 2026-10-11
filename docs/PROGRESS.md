# Progress Log

| Day | Target | Status |
|-----|--------|--------|
| 1 | Toolchain, repo skeleton, CMake, CI, lexer | **done** |
| 2 | Parser, AST, semantic analysis | **done** |
| 3 | AST interpreter (reference oracle) | **done** |
| 4 | Three-address IR, CFG, verifier, IR VM | **done** |
| 5 | Optimization passes and pass manager | **done** |
| 6 | LLVM backend, differential tester, fuzzer, reducer | **done** |
| 7 | ML pass selection, benchmarks, documentation | **done** |

---

## What exists

**Compiler** (C++20, ~6,500 lines)
- Lexer, recursive-descent parser, semantic analysis
- AST interpreter — the reference oracle
- Three-address IR with basic blocks and a CFG
- IR verifier, run after every pass in debug builds
- Five optimization passes with per-pass statistics, independently toggleable
- IR virtual machine with dynamic instruction counting
- LLVM IR text emitter plus a C runtime, compiled by clang
- ML pass selection (`-O-ml`), with inference in C++ and no Python dependency

**Verification** (Python)
- Differential tester across four execution configurations
- Random program generator
- Test-case reducer (delta debugging plus expression simplification)
- pytest suite including golden tests with hand-written expected output

**ML**
- Dataset generator, feature extraction, scikit-learn training, JSON export

---

## Bugs found, and by what

The point of recording these is that each was found by a *different* tool, which
is the argument for having more than one.

| # | Bug | Found by |
|---|---|---|
| 1 | Passing an array to a function was rejected | unit test |
| 2 | `int a[3] = 1;` gave a baffling error message | unit test |
| 3 | The call-depth limit was unreachable — the interpreter crashed before the counter could fire | unit test (a crash, not a failure) |
| 4 | Without `loadelim`, `-O2` improved only 7 of 35 corpus programs | effectiveness test |
| 5 | The pipeline was not idempotent: `cse` ran before `copyprop` | idempotence test |
| 6 | Native exit status wrong on Windows (OS does not truncate to 8 bits) | **fuzzer**, seed 1 |
| 7 | Step-limit aborts shared exit status 70 with real traps | **fuzzer**, seed 5 |
| 8 | `-O-ml` selected the empty pipeline, delivering zero optimization | benchmark comparison |
| 9 | The fix for #8 defeated itself: the excluded empty candidate still set the minimum score, so nothing passed the tolerance filter and `-O-ml` returned an empty pipeline anyway | code review |
| 10 | `ml/data/` was never ignored — `.gitignore` has no inline comments, so the trailing `# ...` became part of the pattern and ~10 MB of datasets were tracked | code review |
| 11 | The candidate sequence list was seeded from `--seed`, so two datasets could not be merged and two models could not be compared | dataset merge failure |

Bugs 6 and 7 are the ones that justify the whole verification layer: neither was
reachable from any hand-written test, and bug 6 is invisible on Linux entirely.

Details and reasoning for each are in [`DESIGN.md`](DESIGN.md) §6.

---

## Measured results

All numbers produced by the tooling in this repo; none estimated.

**Correctness**
- 192 C++ unit tests, including all 120 pass orderings checked against the
  oracle on 35 corpus programs
- 87 pytest tests, including golden tests with hand-written expected output
- Random programs × 4 execution paths, 0 mismatches
- 28,260 (program, pass-sequence) pairs measured during dataset generation,
  0 behaviour changes

**Optimization** (8 benchmarks, dynamic instruction count)
- `-O2` geometric mean ratio **0.9164** — 8.4% fewer instructions
- `-O-ml` geometric mean ratio **0.9168**, matching `-O2` exactly on 7 of 8

**The pass-interaction finding**
- No single pass improves anything on its own: all five score exactly 1.000
- No *pair* does either, including `dce` paired with any other
- `loadelim + copyprop + dce` reaches **0.916** — identical to all twelve
  passes of `-O2`

**Pass selection** (236 held-out programs)
- No candidate sequence beats `-O2` on runtime cost — on any of 942 programs
- `-O-ml` matches `-O2` quality on 215 of 236 while running 4.03 passes
  instead of 12

See the README for the full table and the charts.

---

## Not done

Honest list of what a reader should not expect to find:

- **No SSA form**, so no mem2reg, no global CSE, no LICM. `loadelim` is a
  block-local stand-in.
- **No function inlining**, no loop optimizations, no strength reduction.
- **No bounds-check elimination**, even where provably redundant.
- **Single file only** — no separate compilation or linking.
- **CI has never run.** The workflow is written but there is no remote yet;
  every number above comes from local runs on Windows.
