# Progress Log

| Day | Target | Status |
|-----|--------|--------|
| 1 | Toolchain, repo skeleton, CMake, CI, lexer | in progress |
| 2 | Parser -> AST, type/scope checker | not started |
| 3 | AST interpreter (reference oracle) + test programs | not started |
| 4 | Three-address IR, CFG, lowering, IR verifier, IR VM | not started |
| 5 | 4 optimization passes + pass manager + stats | not started |
| 6 | Differential tester + random program generator (+LLVM emitter) | not started |
| 7 | ML pass ordering, benchmarks, README | not started |

---

## Day 1

### Done
- Repo skeleton: `src/ include/ tools/ tests/ docs/ bench/ ml/ cmake/`
- Root `CMakeLists.txt`, `CMakePresets.json` (debug / asan / release)
- `cmake/CompilerWarnings.cmake`, `cmake/Sanitizers.cmake` (INTERFACE targets)
- Walking skeleton: `mloptc_core` + `mloptc` CLI + first GoogleTest
- `.clang-format` (LLVM base, 100 cols), `format.ps1`
- `.github/workflows/ci.yml`: linux-clang-asan-ubsan, linux-gcc-release,
  windows-clang-debug, plus a clang-format check
- `docs/DESIGN.md` seeded with Phase A rationale

### Blocked
**No working C++ toolchain yet.** Installed: clang 23.1.3, CMake 4.4.4,
Ninja 1.13.2, clang-format 23.1.3. Missing: a C++ standard library and the
Windows SDK import libraries.

Evidence -- `cmake --preset debug` parses all CMake correctly and fails only at
the compiler check:

```
lld-link: error: could not open 'kernel32.lib': no such file or directory
lld-link: error: could not open 'user32.lib': no such file or directory
```

clang on Windows is a compiler *front end*; it defaults to
`--target=x86_64-pc-windows-msvc` and expects Microsoft's C++ library and SDK
underneath. Resolution pending -- see "Toolchain decision" below.

### Toolchain decision
_(to be filled in once chosen)_

### Not yet verified
These are the Phase A "definition of done" items that cannot be checked until
the toolchain is complete:
- [ ] `cmake --preset debug && cmake --build --preset debug && ctest --preset debug`
- [ ] `mloptc --version` prints `MLOpt-C 0.1.0`
- [ ] A deliberate unused-variable warning fails the build
- [ ] A deliberate heap overflow in a test is caught by ASan
- [ ] CI green on a pushed branch
