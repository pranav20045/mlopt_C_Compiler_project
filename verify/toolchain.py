"""Locating and driving the MLOpt-C toolchain from Python.

Everything the verification and ML tooling needs in order to compile and run a
ToyC program lives here, so that no other module has to know where the compiler
binary is or how to invoke clang.

The three execution paths this module exposes are the heart of the project:

    oracle  -- mloptc --run-ast   (the AST interpreter)
    vm      -- mloptc --run-vm    (the IR virtual machine, optionally optimized)
    native  -- mloptc --emit-llvm, then clang, then run the binary

They are independent implementations. Agreement between them is evidence; a
disagreement is a bug in one of them, never an acceptable difference, because
ToyC is specified to have no undefined behaviour (docs/spec.md section 1).
"""

from __future__ import annotations

import dataclasses
import os
import pathlib
import platform
import shutil
import subprocess
import sys
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent

#: Per docs/spec.md 7.6, every runtime error exits with this status.
RUNTIME_ERROR_EXIT = 70

#: A backend gave up because the program ran too long. NOT a runtime error:
#: the program was still running, which the spec allows, and each backend gives
#: up at a different point. Treated as "no result" everywhere.
STEP_LIMIT_EXIT = 124

#: Wall-clock limit for one program. A timeout is "no result", not a mismatch
#: (spec 7.7) -- ToyC programs are allowed to loop forever.
DEFAULT_TIMEOUT_SECONDS = 10.0


class ToolchainError(RuntimeError):
    """Raised when the toolchain is missing or misconfigured."""


@dataclasses.dataclass(frozen=True)
class RunResult:
    """The observable behaviour of one execution.

    Per docs/spec.md section 8, two executions are equivalent iff they agree on
    ``stdout`` and ``exit_status``. Nothing else in this object is compared --
    in particular ``stderr`` is not, since the spec deliberately excludes
    diagnostic text, and the native backend does not print one at all.
    """

    stdout: str
    exit_status: int
    timed_out: bool = False
    stderr: str = ""

    @property
    def trapped(self) -> bool:
        return self.exit_status == RUNTIME_ERROR_EXIT

    @property
    def gave_up(self) -> bool:
        """True if this run produced no verdict: a wall-clock timeout, or a
        backend's internal step limit."""
        return self.timed_out or self.exit_status == STEP_LIMIT_EXIT

    def observably_equals(self, other: "RunResult") -> bool:
        if self.gave_up or other.gave_up:
            # A timeout tells us nothing about equivalence, so it is never
            # reported as a mismatch. Counting it as one would fill the report
            # with false alarms from programs that merely run a long time.
            return True
        return self.stdout == other.stdout and self.exit_status == other.exit_status


def _exe(name: str) -> str:
    return name + ".exe" if platform.system() == "Windows" else name


def find_compiler() -> pathlib.Path:
    """Locates the mloptc binary, preferring a release build."""
    override = os.environ.get("MLOPTC_BINARY")
    if override:
        path = pathlib.Path(override)
        if path.is_file():
            return path
        raise ToolchainError(f"MLOPTC_BINARY points at {override}, which does not exist")

    for preset in ("release", "debug", "asan"):
        candidate = REPO_ROOT / "build" / preset / "bin" / _exe("mloptc")
        if candidate.is_file():
            return candidate

    raise ToolchainError(
        "mloptc not found. Build it first:\n"
        "  cmake --preset release && cmake --build --preset release"
    )


def find_clang() -> str | None:
    """Locates clang, or returns None if the native path is unavailable."""
    found = shutil.which("clang")
    if found:
        return found
    # Windows: clang is often installed but not on PATH in this shell.
    fallback = pathlib.Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "LLVM" / "bin" / "clang.exe"
    return str(fallback) if fallback.is_file() else None


def runtime_source() -> pathlib.Path:
    path = REPO_ROOT / "runtime" / "mloptc_runtime.c"
    if not path.is_file():
        raise ToolchainError(f"runtime source missing: {path}")
    return path


@dataclasses.dataclass
class Toolchain:
    compiler: pathlib.Path
    clang: str | None
    timeout: float = DEFAULT_TIMEOUT_SECONDS

    @classmethod
    def discover(cls, timeout: float = DEFAULT_TIMEOUT_SECONDS) -> "Toolchain":
        return cls(compiler=find_compiler(), clang=find_clang(), timeout=timeout)

    @property
    def has_native(self) -> bool:
        return self.clang is not None

    # -- internals ----------------------------------------------------------

    def _run(self, argv: list[str], timeout: float | None = None) -> RunResult:
        try:
            completed = subprocess.run(
                argv,
                capture_output=True,
                text=True,
                timeout=timeout or self.timeout,
                # Never inherit stdin: a hung read would look like an infinite
                # loop and waste the whole timeout.
                stdin=subprocess.DEVNULL,
            )
        except subprocess.TimeoutExpired:
            return RunResult(stdout="", exit_status=-1, timed_out=True)
        except OSError as exc:  # pragma: no cover - environment failure
            raise ToolchainError(f"failed to run {argv[0]}: {exc}") from exc

        return RunResult(
            # Normalise line endings. On Windows a child process may emit
            # "\r\n"; the spec says print writes "\n", and comparing raw bytes
            # across backends would otherwise fail for a reason that has
            # nothing to do with the compiler.
            stdout=completed.stdout.replace("\r\n", "\n"),
            exit_status=completed.returncode,
            stderr=completed.stderr,
        )

    # -- the three execution paths -------------------------------------------

    def run_oracle(self, source_path: pathlib.Path) -> RunResult:
        """The AST interpreter. Never optimized -- it is the reference."""
        return self._run([str(self.compiler), "--run-ast", str(source_path)])

    def run_vm(self, source_path: pathlib.Path, passes: list[str] | None = None) -> RunResult:
        argv = [str(self.compiler), "--run-vm"]
        argv += ["--passes", ",".join(passes)] if passes else ["-O0"]
        argv.append(str(source_path))
        return self._run(argv)

    def run_native(
        self,
        source_path: pathlib.Path,
        passes: list[str] | None = None,
        clang_opt: str = "-O2",
    ) -> RunResult:
        """Emits LLVM IR, compiles it with clang, and runs the binary.

        ``clang_opt`` defaults to -O2 on purpose. Compiling at -O0 would hide
        exactly the class of bug this path exists to find: a backend that emits
        IR whose meaning depends on undefined behaviour (an `nsw` flag, a raw
        `sdiv`) behaves correctly until the optimizer acts on the assumption.
        """
        if self.clang is None:
            raise ToolchainError("clang not available; the native path cannot run")

        with tempfile.TemporaryDirectory(prefix="mloptc_native_") as tmp:
            tmp_path = pathlib.Path(tmp)
            ll_path = tmp_path / "program.ll"
            exe_path = tmp_path / _exe("program")

            emit = self._run(
                [str(self.compiler), "--emit-llvm"]
                + (["--passes", ",".join(passes)] if passes else ["-O0"])
                + [str(source_path), "-o", str(ll_path)]
            )
            if emit.exit_status != 0 or not ll_path.is_file():
                raise ToolchainError(f"failed to emit LLVM IR: {emit.stderr}")

            compile_result = self._run(
                [
                    self.clang,
                    clang_opt,
                    "-w",  # the generated .ll triggers a target-triple warning
                    str(ll_path),
                    str(runtime_source()),
                    "-o",
                    str(exe_path),
                ],
                timeout=max(self.timeout, 60.0),
            )
            if compile_result.exit_status != 0 or not exe_path.is_file():
                raise ToolchainError(f"clang failed:\n{compile_result.stderr}")

            return self._run([str(exe_path)])

    # -- compiler queries -----------------------------------------------------

    def features(self, source_path: pathlib.Path) -> dict[str, float] | None:
        """Static IR features of the unoptimized program."""
        import json

        result = self._run([str(self.compiler), "--emit-features", str(source_path)])
        line = result.stdout.strip().splitlines()
        if not line:
            return None
        try:
            return json.loads(line[0])
        except ValueError:
            return None

    def measure(self, source_path: pathlib.Path, passes: list[str] | None = None) -> dict | None:
        """Runs on the VM and returns the JSON record, including the dynamic
        instruction count that the ML half of the project optimizes."""
        import json

        argv = [str(self.compiler), "--run-vm", "--json"]
        argv += ["--passes", ",".join(passes)] if passes else ["-O0"]
        argv.append(str(source_path))

        result = self._run(argv)
        if result.timed_out:
            return None
        for line in result.stdout.splitlines():
            line = line.strip()
            if line.startswith("{"):
                try:
                    return json.loads(line)
                except ValueError:
                    return None
        return None

    def compiles(self, source_path: pathlib.Path) -> bool:
        """True if the front end accepts the program."""
        result = self._run([str(self.compiler), "--emit-ir", str(source_path)])
        return result.exit_status == 0 and not result.timed_out

    def pass_names(self) -> list[str]:
        result = self._run([str(self.compiler), "--list-passes"])
        names: list[str] = []
        for line in result.stdout.splitlines():
            stripped = line.strip()
            # Pass names are the two-space-indented entries; their descriptions
            # are indented further.
            if line.startswith("  ") and not line.startswith("      ") and stripped and " " not in stripped:
                names.append(stripped)
        return names


def main() -> int:
    """`python verify/toolchain.py` prints what was found, for troubleshooting."""
    try:
        tc = Toolchain.discover()
    except ToolchainError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    print(f"mloptc : {tc.compiler}")
    print(f"clang  : {tc.clang or '(not found -- native path disabled)'}")
    print(f"passes : {', '.join(tc.pass_names()) or '(none reported)'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
