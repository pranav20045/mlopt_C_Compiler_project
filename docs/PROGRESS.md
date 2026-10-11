# Progress Log

| Day | Target | Status |
|-----|--------|--------|
| 1 | Toolchain, repo skeleton, CMake, CI, lexer | **done** |
| 2 | Parser -> AST, type/scope checker | next |
| 3 | AST interpreter (reference oracle) + test programs | not started |
| 4 | Three-address IR, CFG, lowering, IR verifier, IR VM | not started |
| 5 | 4 optimization passes + pass manager + stats | not started |
| 6 | Differential tester + random program generator (+LLVM emitter) | not started |
| 7 | ML pass ordering, benchmarks, README | not started |

---

## Day 1 -- done

### Delivered
- `docs/spec.md`: the complete ToyC language specification, frozen at v1.0
- Repo skeleton, CMake build graph, `CMakePresets.json` (debug / asan / release)
- `env.ps1`: one-command reproducible toolchain setup on Windows
- Lexer, token definitions, diagnostic engine -- **29 tests, all passing**
- CI workflow, `.clang-format`, `format.ps1`

### Phase A definition of done -- all verified
- [x] `cmake --preset debug && cmake --build --preset debug && ctest --preset debug`
      -> **29/29 passing**
- [x] `mloptc --version` prints `MLOpt-C 0.1.0`
- [x] A deliberate unused variable **fails** the build
      (`error: unused variable 'deliberatelyUnused' [-Werror,-Wunused-variable]`)
- [x] A deliberate heap overflow in a test **is caught** by ASan
      (`heap-buffer-overflow ... test_version.cpp:16`), with exact file and line
- [x] ASan build: **29/29 passing**
- [x] `clang-format --dry-run --Werror`: clean
- [ ] CI green on a pushed branch -- *no remote configured yet*

### Toolchain decision and what it cost
Windows, with **LLVM/clang 23.1.3 + CMake 4.4.4 + Ninja 1.13.2 + MSVC Build
Tools 17.14 + Windows SDK 10.0.26100**. Linux is not needed locally; GitHub
Actions provides it for the authoritative sanitizer run.

Four problems were hit and fixed, each recorded in `docs/DESIGN.md`:

1. **clang alone is not a toolchain on Windows.** It targets
   `x86_64-pc-windows-msvc` and needs the MSVC standard library and the Windows
   SDK import libraries underneath. Symptom: `'iostream' file not found`, then
   `lld-link: could not open 'kernel32.lib'`.
2. **clang does not auto-detect a Build Tools-only install.** Resolved by
   importing the `vcvars64.bat` environment in `env.ps1`.
3. **ASan is a whole-program property, not a per-target one.** Instrumenting our
   code but not GoogleTest changed `std::string`'s layout between them, and the
   linker refused: `/failifmismatch: mismatch detected for 'annotate_string'`.
   Sanitizer flags are now global; warning flags remain per-target.
4. **ASan on Windows is incompatible with the debug CRT.** Linking
   `ucrtbased.dll` aborted inside CRT startup before `main`. Fixed by forcing
   the release CRT in sanitizer builds.

Note that the installer itself exited 1603, but the components we need all
installed correctly -- verified directly on disk rather than trusting the exit
code.

### Next
Day 2: parser and AST. The lexer's contract (always exactly one trailing
`EndOfFile` token, never fails, reports and recovers) is what the parser will
be built against.
