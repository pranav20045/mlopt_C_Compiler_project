"""The differential tester: the project's central verification tool.

It compiles and runs the same program along several independent paths and
reports any disagreement. Because ToyC has no undefined behaviour
(docs/spec.md section 1), there is no such thing as an acceptable difference:
every mismatch is a bug in one of the implementations.

Paths compared, each adding a different kind of evidence:

    oracle          AST interpreter, never optimized -- the reference
    vm-O0           IR VM, no passes          -> checks lowering and the VM
    vm-<pipeline>   IR VM, optimized          -> checks the optimizer
    native-O0       clang-compiled, no passes -> checks the LLVM backend
    native-<pipe>   clang-compiled, optimized -> checks both together

The native paths compile with `clang -O2` even when mloptc applied no passes of
its own. That is deliberate: a backend bug that emits IR relying on undefined
behaviour (an `nsw` flag, a raw `sdiv`) produces correct code until an optimizer
acts on the assumption. Only running clang's optimizer exposes it.

Usage:
    python verify/differential.py --count 1000
    python verify/differential.py --count 10000 --jobs 8 --seed 42
    python verify/differential.py --file path/to/program.tc
"""

from __future__ import annotations

import argparse
import concurrent.futures
import dataclasses
import json
import pathlib
import sys
import tempfile
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

from verify.generator import generate_program  # noqa: E402
from verify.toolchain import RunResult, Toolchain, ToolchainError  # noqa: E402


@dataclasses.dataclass
class Mismatch:
    seed: int | None
    source: str
    path_a: str
    path_b: str
    result_a: RunResult
    result_b: RunResult

    def describe(self) -> str:
        def summarise(name: str, result: RunResult) -> str:
            shown = result.stdout if len(result.stdout) <= 400 else result.stdout[:400] + "...<truncated>"
            return f"  {name}: exit={result.exit_status}\n    stdout={shown!r}"

        return (
            f"MISMATCH between {self.path_a} and {self.path_b}"
            + (f" (seed {self.seed})" if self.seed is not None else "")
            + "\n"
            + summarise(self.path_a, self.result_a)
            + "\n"
            + summarise(self.path_b, self.result_b)
        )


@dataclasses.dataclass
class Outcome:
    seed: int | None
    checked: int = 0
    skipped_reason: str | None = None
    mismatches: list[Mismatch] = dataclasses.field(default_factory=list)
    timed_out: bool = False
    trapped: bool = False


def check_program(
    tc: Toolchain,
    source: str,
    *,
    seed: int | None = None,
    pipelines: list[list[str]] | None = None,
    include_native: bool = True,
) -> Outcome:
    """Runs one program along every path and compares against the oracle."""
    outcome = Outcome(seed=seed)

    if pipelines is None:
        pipelines = [[], ["loadelim", "copyprop", "constfold", "cse", "copyprop", "dce"]]

    with tempfile.TemporaryDirectory(prefix="mloptc_diff_") as tmp:
        path = pathlib.Path(tmp) / "program.tc"
        path.write_text(source, encoding="utf-8")

        # A program the front end rejects is not evidence of anything -- the
        # generator is allowed to be imperfect, as long as it never produces a
        # program that compiles and then misbehaves.
        if not tc.compiles(path):
            outcome.skipped_reason = "rejected by the front end"
            return outcome

        oracle = tc.run_oracle(path)
        if oracle.gave_up:
            # Spec 7.7: a program may run forever, and that is legal. Neither a
            # wall-clock timeout nor a step-limit abort says anything about
            # whether the backends agree, so the program is skipped rather than
            # reported.
            outcome.timed_out = True
            outcome.skipped_reason = "oracle did not terminate"
            return outcome

        outcome.trapped = oracle.trapped

        results: list[tuple[str, RunResult]] = []

        for passes in pipelines:
            label = "vm-O0" if not passes else "vm-opt"
            try:
                results.append((label, tc.run_vm(path, passes or None)))
            except ToolchainError as exc:
                outcome.skipped_reason = f"vm failed: {exc}"
                return outcome

        if include_native and tc.has_native:
            for passes in pipelines:
                label = "native-O0" if not passes else "native-opt"
                try:
                    results.append((label, tc.run_native(path, passes or None)))
                except ToolchainError as exc:
                    outcome.skipped_reason = f"native failed: {exc}"
                    return outcome

        for label, result in results:
            outcome.checked += 1
            if not oracle.observably_equals(result):
                outcome.mismatches.append(
                    Mismatch(
                        seed=seed,
                        source=source,
                        path_a="oracle",
                        path_b=label,
                        result_a=oracle,
                        result_b=result,
                    )
                )

    return outcome


def _worker(args: tuple[int, str, int, bool]) -> Outcome:
    seed, size, max_depth, include_native = args
    # Each worker discovers the toolchain itself: a Toolchain holds paths only,
    # so there is nothing to share and nothing to synchronise.
    tc = Toolchain.discover()
    return check_program(
        tc, generate_program(seed, size=size, max_depth=max_depth), seed=seed, include_native=include_native
    )


def run_campaign(
    count: int,
    *,
    seed: int = 0,
    size: str = "medium",
    max_depth: int = 3,
    jobs: int = 1,
    include_native: bool = True,
    stop_after: int = 5,
    report_path: pathlib.Path | None = None,
    quiet: bool = False,
) -> int:
    """Generates and checks `count` programs. Returns the number of mismatches."""
    tc = Toolchain.discover()
    if include_native and not tc.has_native:
        print("warning: clang not found; the native paths are disabled", file=sys.stderr)
        include_native = False

    print(f"compiler : {tc.compiler}")
    print(f"native   : {'enabled' if include_native else 'disabled'}")
    print(f"programs : {count} (seeds {seed}..{seed + count - 1}), {jobs} job(s)\n")

    started = time.time()
    tasks = [(seed + i, size, max_depth, include_native) for i in range(count)]

    total_checked = 0
    skipped = 0
    timeouts = 0
    trapping = 0
    all_mismatches: list[Mismatch] = []

    def handle(outcome: Outcome, done: int) -> bool:
        nonlocal total_checked, skipped, timeouts, trapping
        total_checked += outcome.checked
        if outcome.skipped_reason:
            skipped += 1
        if outcome.timed_out:
            timeouts += 1
        if outcome.trapped:
            trapping += 1
        all_mismatches.extend(outcome.mismatches)

        for mismatch in outcome.mismatches:
            print("\n" + mismatch.describe(), file=sys.stderr)
            print("--- source ---\n" + mismatch.source, file=sys.stderr)

        if not quiet and (done % 100 == 0 or done == count):
            elapsed = time.time() - started
            rate = done / elapsed if elapsed > 0 else 0.0
            print(
                f"\r{done}/{count} programs | {total_checked} comparisons | "
                f"{len(all_mismatches)} mismatches | {rate:.1f}/s",
                end="",
                flush=True,
            )
        return len(all_mismatches) < stop_after

    if jobs > 1:
        with concurrent.futures.ProcessPoolExecutor(max_workers=jobs) as pool:
            for done, outcome in enumerate(pool.map(_worker, tasks), start=1):
                if not handle(outcome, done):
                    break
    else:
        for done, task in enumerate(tasks, start=1):
            if not handle(_worker(task), done):
                break

    elapsed = time.time() - started
    print("\n")
    print("=" * 60)
    print(f"programs generated : {count}")
    print(f"comparisons made   : {total_checked}")
    print(f"skipped            : {skipped}")
    print(f"timed out          : {timeouts}")
    print(f"trapped (expected) : {trapping}")
    print(f"MISMATCHES         : {len(all_mismatches)}")
    print(f"elapsed            : {elapsed:.1f}s")
    print("=" * 60)

    if report_path is not None:
        report = {
            "programs": count,
            "comparisons": total_checked,
            "skipped": skipped,
            "timeouts": timeouts,
            "trapping": trapping,
            "mismatches": [
                {
                    "seed": m.seed,
                    "paths": [m.path_a, m.path_b],
                    "source": m.source,
                    "a": {"stdout": m.result_a.stdout, "exit": m.result_a.exit_status},
                    "b": {"stdout": m.result_b.stdout, "exit": m.result_b.exit_status},
                }
                for m in all_mismatches
            ],
            "elapsed_seconds": elapsed,
        }
        report_path.parent.mkdir(parents=True, exist_ok=True)
        report_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
        print(f"report written to {report_path}")

    if all_mismatches:
        print("\nReproduce the first failure with:")
        print(f"  python verify/generator.py --seed {all_mismatches[0].seed} --size {size}")
        print(f"  python verify/reducer.py --seed {all_mismatches[0].seed} --size {size}")

    return len(all_mismatches)


def main() -> int:
    parser = argparse.ArgumentParser(description="Differential-test the MLOpt-C execution paths.")
    parser.add_argument("--count", type=int, default=100, help="number of random programs")
    parser.add_argument("--seed", type=int, default=0, help="first seed")
    parser.add_argument("--size", choices=["small", "medium", "large"], default="medium")
    parser.add_argument("--max-depth", type=int, default=3)
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument("--no-native", action="store_true", help="skip the clang-compiled paths")
    parser.add_argument("--stop-after", type=int, default=5, help="stop after this many mismatches")
    parser.add_argument("--report", type=pathlib.Path, help="write a JSON report here")
    parser.add_argument("--file", type=pathlib.Path, help="check one specific program instead")
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()

    try:
        tc = Toolchain.discover()
    except ToolchainError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    if args.file:
        source = args.file.read_text(encoding="utf-8")
        outcome = check_program(tc, source, include_native=not args.no_native)
        if outcome.skipped_reason:
            print(f"skipped: {outcome.skipped_reason}")
            return 0
        for mismatch in outcome.mismatches:
            print(mismatch.describe(), file=sys.stderr)
        print(f"{outcome.checked} comparisons, {len(outcome.mismatches)} mismatches")
        return 1 if outcome.mismatches else 0

    mismatches = run_campaign(
        args.count,
        seed=args.seed,
        size=args.size,
        max_depth=args.max_depth,
        jobs=args.jobs,
        include_native=not args.no_native,
        stop_after=args.stop_after,
        report_path=args.report,
        quiet=args.quiet,
    )
    return 1 if mismatches else 0


if __name__ == "__main__":
    raise SystemExit(main())
