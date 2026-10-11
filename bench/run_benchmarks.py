"""Runs the benchmark suite and records the numbers the README reports.

For each benchmark, every optimization level is measured and -- critically --
checked against the oracle. A benchmark result from a pipeline that changed the
program's behaviour is worthless, so correctness is verified before any number
is recorded.

The cost metric is DYNAMIC INSTRUCTION COUNT from the IR VM, not wall-clock
time. The reasons are worth stating, because the choice shapes everything
downstream:

  * It is exactly reproducible. Wall-clock time on a laptop varies by 10-20%
    between runs, which is larger than most of the effects being measured --
    a model trained on it would be learning scheduler noise.
  * It is machine-independent, so the chart in the README means the same thing
    on the reader's machine as on mine.
  * It is a PROXY, not a measurement. It counts a division the same as an add,
    and it ignores cache behaviour entirely. The README says so.

Usage:
    python bench/run_benchmarks.py --out bench/results/results.json
"""

from __future__ import annotations

import argparse
import json
import pathlib
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

from verify.toolchain import Toolchain, ToolchainError  # noqa: E402

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
PROGRAM_DIR = REPO_ROOT / "bench" / "programs"

O2_PIPELINE = [
    "loadelim", "copyprop", "constfold", "cse", "copyprop", "dce",
    "loadelim", "copyprop", "constfold", "cse", "copyprop", "dce",
]

SINGLE_PASSES = ["loadelim", "constfold", "copyprop", "cse", "dce"]


def run_one(tc: Toolchain, path: pathlib.Path, model: pathlib.Path | None) -> dict | None:
    print(f"  {path.stem:<16}", end="", flush=True)

    oracle = tc.run_oracle(path)
    if oracle.gave_up:
        print("skipped (did not terminate)")
        return None

    baseline = tc.measure(path, None)
    if baseline is None:
        print("skipped (no baseline)")
        return None

    record: dict = {
        "name": path.stem,
        "baseline_dynamic": baseline["dynamic_instructions"],
        "baseline_static": baseline["static_instructions_before"],
        "expected_output": oracle.stdout,
        "levels": {},
    }

    configurations: list[tuple[str, list[str] | None]] = [
        ("O0", None),
        ("O2", O2_PIPELINE),
    ]
    for name in SINGLE_PASSES:
        configurations.append((f"only-{name}", [name]))

    for label, passes in configurations:
        measured = tc.measure(path, passes)
        if measured is None:
            continue

        # A benchmark number from a miscompiling pipeline is meaningless.
        if measured["output"] != oracle.stdout or measured["exit_status"] != oracle.exit_status:
            print(f"\n    !! {label} CHANGED BEHAVIOUR -- not recorded")
            record.setdefault("miscompiles", []).append(label)
            continue

        record["levels"][label] = {
            "dynamic": measured["dynamic_instructions"],
            "static": measured["static_instructions"],
            "ratio": measured["dynamic_instructions"] / max(1, baseline["dynamic_instructions"]),
        }

    # -O-ml only if a model exists; a missing model is not an error.
    if model is not None and model.is_file():
        import subprocess

        try:
            completed = subprocess.run(
                [str(tc.compiler), "--run-vm", "--json", "-O-ml", "--model", str(model), str(path)],
                capture_output=True, text=True, timeout=tc.timeout, stdin=subprocess.DEVNULL,
            )
            for line in completed.stdout.splitlines():
                if line.strip().startswith("{"):
                    measured = json.loads(line)
                    if measured["output"] == oracle.stdout:
                        record["levels"]["O-ml"] = {
                            "dynamic": measured["dynamic_instructions"],
                            "static": measured["static_instructions"],
                            "ratio": measured["dynamic_instructions"]
                            / max(1, baseline["dynamic_instructions"]),
                        }
                    else:
                        print("\n    !! -O-ml CHANGED BEHAVIOUR -- not recorded")
                    break
        except (subprocess.TimeoutExpired, ValueError):
            pass

    o2 = record["levels"].get("O2", {}).get("ratio")
    print(f"baseline={record['baseline_dynamic']:>10,}  -O2 ratio={o2:.3f}" if o2 else "no -O2 result")
    return record


def main() -> int:
    parser = argparse.ArgumentParser(description="Run the MLOpt-C benchmark suite.")
    parser.add_argument("--out", type=pathlib.Path, default=REPO_ROOT / "bench" / "results" / "results.json")
    parser.add_argument("--model", type=pathlib.Path, default=REPO_ROOT / "ml" / "models" / "model.json")
    parser.add_argument("--timeout", type=float, default=120.0)
    args = parser.parse_args()

    try:
        tc = Toolchain.discover(timeout=args.timeout)
    except ToolchainError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    programs = sorted(PROGRAM_DIR.glob("*.tc"))
    if not programs:
        print(f"error: no benchmarks in {PROGRAM_DIR}", file=sys.stderr)
        return 2

    print(f"compiler : {tc.compiler}")
    print(f"model    : {args.model if args.model.is_file() else '(none -- -O-ml skipped)'}")
    print(f"\nrunning {len(programs)} benchmarks:")

    started = time.time()
    records = [r for r in (run_one(tc, p, args.model) for p in programs) if r is not None]

    results = {
        "benchmarks": records,
        "o2_pipeline": O2_PIPELINE,
        "elapsed_seconds": time.time() - started,
    }

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(results, indent=2), encoding="utf-8")

    print(f"\nwritten to {args.out}")

    # A geometric mean, not an arithmetic one: these are ratios, and averaging
    # ratios arithmetically overweights the ones that got worse.
    import math

    for label in ("O2", "O-ml"):
        ratios = [r["levels"][label]["ratio"] for r in records if label in r["levels"]]
        if ratios:
            geo = math.exp(sum(math.log(x) for x in ratios) / len(ratios))
            print(f"geometric mean ratio, -{label}: {geo:.4f}  ({100 * (1 - geo):+.1f}% instructions)")

    miscompiled = [r["name"] for r in records if r.get("miscompiles")]
    if miscompiled:
        print(f"\n!! pipelines changed behaviour on: {', '.join(miscompiled)}")
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
