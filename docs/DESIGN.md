# MLOpt-C — Design Notes

A record of the design decisions in this project and *why* they were made,
including the ones that turned out to be wrong.

---

## 1. What the project is

A small optimizing compiler for **ToyC** (a C-like language) with three
independent execution paths and an ML-guided optimization pass scheduler.

```
source.tc
   |-- Lexer -> Parser -> AST -> Semantic analysis
   |                               |
   |                               |-- AST interpreter ........ REFERENCE ORACLE
   |                               |
   |                               '-- Lowering to three-address IR (+ CFG)
   |                                          |
   |                                          |-- IR verifier (after every pass)
   |                                          |-- Pass manager -> optimizations
   |                                          |
   |                                          |-- IR VM (counts instructions)
   |                                          '-- LLVM IR text -> clang -> native
```

The central claim is that all three paths agree on every program, and the
evidence is a differential tester that runs thousands of randomly generated
programs through all of them.

---

## 2. The decision everything else rests on: no undefined behaviour

ToyC is specified so that **every program has exactly one defined meaning**
(docs/spec.md §1, §7). Overflow wraps, division by zero traps, arrays are
bounds-checked, uninitialised variables are zero, argument evaluation order is
fixed.

This is not language-design fussiness; it is what makes the verification
strategy possible at all. If ToyC had undefined behaviour, a randomly generated
program could trigger it, and the three implementations could *legitimately*
produce different results. Every such case would be a false alarm needing manual
triage, and a fuzzing campaign would drown in them.

With UB defined away, **any disagreement is a real bug**. That is what lets the
fuzzer run unattended, and it is why trapping programs are a deliberate part of
the test distribution rather than something to filter out.

The cost is paid in the backend: every array access carries a bounds check and
every division carries two, so ToyC code is slower than the equivalent C. That
was the right trade for a project whose subject is correctness.

---

## 3. Front end

### Tagged AST hierarchy, not a visitor
Each node carries a `kind` enum and derived types are reached with
`static_cast`. This is what Clang itself does. With `-Wswitch -Werror`, adding a
node kind and forgetting it anywhere becomes a build error — effectively a type
checker for the compiler. A visitor interface would need updating in parallel
and would not give that guarantee.

### Lexing never fails
Illegal input is reported to the `DiagnosticEngine` and emitted as
`TokenKind::Unknown`, so the parser still receives a well-formed stream. One run
reports many errors instead of one per rebuild.

### No `<cctype>`
`std::isalpha` and friends take an `int` that must be representable as
`unsigned char`. Passing a plain `char` ≥ 0x80 — negative on most platforms — is
undefined behaviour. The lexer uses hand-written ASCII predicates, and a test
(`HighByteDoesNotCrash`) fails under UBSan if anyone reintroduces the library
versions.

### Parser tracks nesting depth
A hand-written recursive-descent parser overflows the stack on input like
`((((((...))))))`. The depth counter turns that into a diagnostic. A crash is
not an acceptable response to bad input.

### Semantic analysis resolves everything once
Every name becomes a `(scope, slot)` pair and every call a function index, so no
later stage looks a name up again. Each function gets a flat frame; shadowed and
out-of-scope locals each get their own slot. Lexical scoping is resolved
entirely at compile time and nothing about scopes survives into execution.

### `for` is a distinct AST node, not sugar
The obvious desugaring `for (init; cond; update) body` →
`{ init; while (cond) { body; update; } }` is **wrong**, because `continue` then
jumps to the condition and skips the update — turning an ordinary counting loop
into an infinite one. Keeping `for` as its own node means each backend handles
`continue` explicitly. The corpus has a test for exactly this.

---

## 4. IR

### Three-address code, basic blocks, CFG — but not SSA
Local variables live in memory slots, accessed with `LoadLocal`/`StoreLocal`.
No phi nodes, no dominator tree, no mem2reg. This was a deliberate scope cut.

What we *do* guarantee is that **every `ValueId` is assigned exactly once**, and
the IR verifier enforces it. That single property is what the optimization
passes actually need: it makes "the value in register `%r`" a well-defined,
position-independent fact, so constant folding and copy propagation are valid
across the whole function with no dataflow analysis.

**The bill for skipping SSA came due immediately.** See §6.

### DCE must not remove trapping instructions
`Div`, `Rem` and `ArrayLoad` produce values *and* can trap. If
`x = a / b` has an unused result, deleting it looks free — but if `b` was zero,
the program was supposed to exit with status 70. **Removing a trap is a
miscompilation**, exactly as much as computing a wrong number, and it is
invisible to any test suite made only of well-behaved programs.
`ir::mayHaveSideEffects` therefore returns true for all three. The corpus
contains `unused_division_must_still_trap` and
`unused_array_load_must_still_trap` to guard it.

### CSE is block-local on purpose
Reusing a computation from another block requires the earlier block to
*dominate* the later one; otherwise some path reaches the use without having
executed the definition. Proving domination needs a dominator tree, which this
compiler does not build. Restricting to one block makes domination trivially
true. This is a real limitation, not a shortcut around correctness.

### Constant folding shares the runtime's arithmetic helpers
`wrapAdd`, `checkedDiv` and friends live in `execution.hpp` and are used by the
interpreter, the VM **and** the folder. A folder using the host's `int`
arithmetic would invoke undefined behaviour inside our own compiler and could
produce a constant that disagrees with what the program computes at runtime.
Sharing the helpers makes them agree by construction.

It also must **not** fold `1 / 0` or `INT_MIN / -1`: those traps are observable
behaviour, so the division has to survive to runtime. Similarly `x * 0` folds to
`0` (multiplication cannot trap) but `0 / x` does **not** (x may be zero).

### The IR verifier runs after every pass in debug builds
It catches a different class of bug from the differential tester. The
differential tester finds passes that change what a program *computes*; the
verifier finds passes that leave the IR *malformed* — a block with two
terminators, a jump to a deleted block, a use before definition. Those often do
not change the answer on the test that happens to run, and surface much later.
Running it per pass means a broken pass is named on the spot.

---

## 5. Backends

### Two backends that share almost nothing
The AST interpreter and the IR VM share only the arithmetic helpers. Two
implementations that shared their logic would agree with each other even when
both were wrong, which would make the differential test worthless.

### LLVM IR as TEXT, with a C runtime
We emit `.ll` text and hand it to `clang`, rather than linking the LLVM C++ API.
That keeps the project dependency-free and makes the output readable when a
backend bug has to be diagnosed. The runtime helpers (bounds checks, trap
semantics, the array arena) are written in C rather than hand-written LLVM IR,
so the tricky parts stay reviewable.

### No `nsw` / `nuw`, ever
ToyC arithmetic wraps. LLVM's `nsw` means "assume signed overflow never
happens", which licenses the optimizer to fold things like `x + 1 > x` to
`true`. Emitting it would produce a binary that disagrees with the oracle on
exactly the programs that overflow — a miscompilation introduced by the backend,
invisible at `-O0`, appearing only under optimization. A test asserts no `nsw`
appears for any corpus program at either level.

For the same reason division goes through a checked helper rather than a raw
`sdiv` (undefined on a zero divisor), and array access through a bounds-checked
helper rather than a raw `getelementptr`.

### Array handles are integers in all three backends
An array is referred to by an integer index into an arena, not a pointer. That
lets a handle live in an ordinary `i32` frame slot and be passed as an ordinary
`i32` argument, which is what gives array parameters reference semantics without
the IR needing a pointer type. The C runtime mirrors the arena so lifetimes
match.

---

## 6. Things that were wrong, and what fixed them

These are kept because the reasoning is more useful than the conclusion.

### The call-depth limit was unreachable
The spec originally set it at 10,000 frames. The AST interpreter implements a
ToyC call as native C++ recursion, so one ToyC frame costs several host frames —
it overflowed the stack and **crashed** before the counter could report a clean
error. The guard failed precisely when it was needed.

Fixed by lowering the limit to 1,000 and linking executables with a 32 MB stack.
A crash and a diagnosed error are not interchangeable here: the differential
tester has to tell them apart.

### Without `loadelim`, the optimizer was nearly blind
In a non-SSA IR, every variable read is a `LoadLocal` producing a **fresh**
register. So the obvious redundancy in `int p = x * y; int q = x * y;` is
invisible to CSE, which sees `mul %0, %1` and `mul %3, %4`. Constant folding
cannot see through a variable either, and copy propagation has nothing to chase.

**Before `loadelim`, `-O2` improved 7 of 35 corpus programs.** The pass forwards
stored values to later loads and reuses repeated loads within a block — the
block-local stand-in for mem2reg — and it is now the most important pass in the
pipeline. This is the cost of skipping SSA, paid in full.

### The pipeline was not idempotent
Running `-O2` twice kept finding redundancy the first run had missed. The cause
was ordering: `cse` ran before `copyprop`, so the `move` instructions `loadelim`
leaves behind had not been forwarded yet and equivalent expressions still had
different operand registers. `copyprop` now runs before `cse`.

This is precisely the kind of insight the ML scheduler is supposed to discover
on its own, and it was found by an idempotence test rather than by inspection.

### The native exit status was wrong on Windows
Found by the fuzzer on seed 1. The spec defines the exit status as `main`'s
return value mod 256. The oracle and VM computed that themselves; the LLVM
backend left it to the OS. POSIX truncates an exit code to 8 bits, so it works
on Linux — but **Windows returns the full 32-bit value**, so
`return -2147483648` exited with `-2147483648` natively while the other paths
reported `0`.

A platform-dependent miscompilation that Linux-only CI would never have caught.
The reduction now happens in the emitted code.

### A step limit is not a trap
Found by the fuzzer on seed 5. A step-limit abort used exit status 70 — the same
status as a real ToyC trap. So a non-terminating program was indistinguishable
from a trapping one, and since each backend gives up at a different point (the
interpreter counts node visits, the VM counts IR instructions, native code has
no counter), every infinite loop looked like a disagreement.

The spec explicitly allows a program to run forever (§7.7), so this is "no
result", never a mismatch. Step-limit aborts now exit 124 and the tester treats
that like a wall-clock timeout.

### A guard that defeated itself

`-O-ml` was observed selecting the *empty* pipeline on
`bench/programs/collatz.tc`, delivering no optimization at all. The empty
sequence is in the candidate set because the dataset needs it as the cost
baseline, and it is also the shortest — so once "prefer the shortest among
indistinguishable candidates" was added, it won whenever the model was unsure.

The first fix skipped it in the selection loop. That was not enough: the
minimum score was still taken over *all* candidates including the empty one, so
when the empty sequence was the strict minimum, nothing satisfied
`score <= bestScore + tolerance` and the function returned `{}` anyway. **The
guard failed in exactly the case it was written for.** The exclusion has to
happen before the minimum is taken.

The unit test written for the first fix could not catch the second bug: its
single-leaf tree gave every candidate an identical score, so the minimum was
always attainable. The regression test now uses a tree that splits on a
pass-count feature, and it was confirmed to fail against the old code.

### No single pass does anything

The benchmark suite showed something that is obvious in hindsight and was not
obvious in advance: **each of the five passes, run alone, achieves exactly
nothing** — and so does every *pair*.

Each pass rewrites instructions rather than removing them. `loadelim` turns a
redundant load into a move, `cse` turns a redundant computation into a move,
`copyprop` rewrites operands. Nothing shrinks until `dce` runs, and `dce` alone
has nothing to remove.

It takes all three of `loadelim → copyprop → dce` before anything happens:
`loadelim` creates the moves, `copyprop` forwards them so the originals become
unused, and only then does `dce` have something to delete. Those three reach
0.916 — identical to all twelve passes of `-O2`.

That measurement is what redirected the ML objective from pass ordering (no
headroom) to compile cost (a factor of three).

### Distribution shift, diagnosed and fixed

The first model trained only on small random programs and generalised badly to
the hand-written benchmarks, which are larger and far more loop-heavy. On
`binary_search` and `fib_recursive` it dropped `loadelim` — the one pass that
matters — and delivered zero improvement.

The fix was to widen the training distribution rather than special-case the
benchmarks: a second dataset of larger, deeper programs, merged with the first,
taking the training set from 537 to 942 programs. The retrained model matches
`-O2` exactly on 7 of 8 benchmarks.

Merging exposed a further bug: the candidate sequence list was seeded from
`--seed`, so two datasets generated with different program seeds had different
*action spaces* and could not be combined — and two models trained on them could
not have been compared. The action space is now seeded independently.

---

## 7. Build system

### Warnings are per-target; sanitizers are per-program
`project_warnings` is an INTERFACE target that only our own code links, so
`-Werror` never applies to GoogleTest. Sanitizers are the opposite: ASan changes
the memory layout of `std::string`, so instrumenting our code but not
GoogleTest is an ODR violation the linker correctly rejects
(`/failifmismatch: mismatch detected for 'annotate_string'`). Sanitizer flags
are therefore global.

### ASan on Windows cannot use the debug CRT
Linking `ucrtbased.dll` aborts inside CRT startup, before `main`, with a stack
trace containing none of our code. Sanitizer builds force the release CRT; the
debug heap's checks are a strict subset of what ASan does anyway.

### clang alone is not a toolchain on Windows
It targets `x86_64-pc-windows-msvc` and needs the MSVC standard library and
Windows SDK import libraries underneath. `env.ps1` imports the `vcvars64.bat`
environment, because clang does not reliably auto-detect a Build Tools-only
install.

### Linux CI is the sanitizer authority
UBSan's runtime support on Windows is incomplete, so the Windows build enables
ASan only and the GitHub Actions Ubuntu runner does the full
`-fsanitize=address,undefined` run.

---

## 8. The ML layer

### The target is a ratio, in log space
The model predicts `log(cost_with_sequence / cost_with_no_passes)`.

A model trained on raw instruction counts mostly learns program *size* — a big
program costs more regardless of the optimizer, so predicting size is the
easiest way to reduce the loss and the pass sequence barely matters to it.
Predicting cost relative to unoptimized removes size from the target entirely.
It is also what lets a model trained on small programs say anything about large
ones.

Log space because ratios are multiplicative: halving twice should be twice the
distance of halving once. In linear space the model would be pushed to care far
more about programs it makes slower than about ones it makes faster.

### The train/test split is by program, not by row
Each program contributes one row per candidate sequence. Splitting rows at
random would put the same program on both sides, and the model could score well
by memorising it. Grouping by program is the difference between a believable
held-out number and a meaningless one.

### The headline metric is selection quality, not R²
What matters is: when the model picks a sequence for an unseen program, how does
that sequence perform against the fixed pipeline? A model can have a mediocre R²
and still rank candidates well — which is all the compiler needs — and it can
have a good R² while picking badly. The evaluation also reports the *best known*
sequence, so the result shows how much of the available win was captured rather
than only whether `-O2` was beaten.

### Inference is in C++, with no Python at compile time
`-O-ml` loads a JSON model and walks the trees in C++. A hand-written strict
JSON parser (~200 lines) keeps the no-dependencies rule. This matters for the
claim being made: `-O-ml` is a real compiler flag, not a Python script driving
the compiler from outside.

The model refuses to load if its feature list does not match the build. A
mismatched model would still load and still predict — from the wrong numbers —
producing a scheduler that quietly picks badly. Refusing is far better than
debugging that later.

### Dynamic instruction count, not wall-clock time
It is exactly reproducible, where laptop timings vary by more than the effects
being measured; a model trained on wall-clock time would be learning scheduler
noise. It is also machine-independent, so the README's chart means the same
thing on the reader's machine.

It is a **proxy**, not a measurement: it counts a division the same as an add
and ignores cache behaviour entirely. The README says so.

---

## 9. Known limitations

- **No SSA, so no mem2reg, LICM, or global CSE.** `loadelim` recovers much of
  what mem2reg would give, but only within a basic block — a value stored before
  a loop is reloaded inside it.
- **No function inlining**, so `fib_recursive` is dominated by call overhead
  that no pass can touch.
- **No loop optimizations at all** (unrolling, LICM, strength reduction), which
  is where most of the remaining headroom on the benchmark suite lies.
- **The native backend caps live arrays at 65,536.** The interpreter and VM grow
  their arena without limit.
- **Single file only.** No separate compilation or linking.
- **Bounds checks are never eliminated**, even when provably redundant.
