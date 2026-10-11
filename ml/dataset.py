"""Builds the training dataset for the pass-ordering model.

For each (program, pass sequence) pair we record:

    features    static IR features of the UNOPTIMIZED program
    sequence    the pass sequence that was applied
    cost        dynamic instructions executed after applying it
    baseline    dynamic instructions with no passes at all
    ratio       cost / baseline   <- the regression target

**Why the target is a ratio and not the raw count.** A model trained on raw
counts mostly learns program size: a big program costs more no matter what the
optimizer does, so the easiest way to reduce error is to predict size, and the
pass sequence barely matters to the loss. Predicting cost RELATIVE to no
optimization removes size from the target entirely, leaving only the question
we actually care about -- how much does this sequence help *this* program. It is
also what makes a model trained on small programs transfer to larger ones.

We train on log(ratio). Ratios are multiplicative (halving twice is the same
distance as halving once, twice over), so squared error in log space is the
natural loss; in linear space a model would be pushed to care far more about
programs it makes slower than about ones it makes faster.

Usage:
    python ml/dataset.py --programs 400 --sequences 24 --out ml/data/dataset.json
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import math
import pathlib
import random
import sys
import tempfile
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

from verify.generator import generate_program  # noqa: E402
from verify.toolchain import Toolchain, ToolchainError  # noqa: E402

ALL_PASSES = ["loadelim", "constfold", "copyprop", "cse", "dce"]

#: The fixed pipeline, included in every dataset so the model always has the
#: baseline it is being compared against inside its candidate set. Without it,
#: -O-ml could "win" simply by never being offered the option that beats it.
O2_PIPELINE = ["loadelim", "copyprop", "constfold", "cse", "copyprop", "dce",
               "loadelim", "copyprop", "constfold", "cse", "copyprop", "dce"]


def random_sequence(rng: random.Random, max_length: int = 12) -> list[str]:
    """A random pass sequence, with repetition allowed.

    Repetition is allowed on purpose: real pipelines run passes more than once
    because each pass exposes work for the others, and a model that could only
    consider permutations would never discover that.
    """
    length = rng.randint(1, max_length)
    return [rng.choice(ALL_PASSES) for _ in range(length)]


def candidate_sequences(rng: random.Random, count: int) -> list[list[str]]:
    """The sequences every program in the dataset is measured against.

    The same set is used for every program, which matters: the model's job at
    inference time is to rank these candidates for an unseen program, so it has
    to have seen each of them on many programs during training.
    """
    sequences: list[list[str]] = [[], list(O2_PIPELINE)]

    # A few hand-built sequences that encode the orderings a compiler engineer
    # would try, so the model is not relying purely on random search to find a
    # sensible pipeline.
    sequences.append(["loadelim", "copyprop", "constfold", "dce"])
    sequences.append(["loadelim", "copyprop", "cse", "copyprop", "dce"])
    sequences.append(["constfold", "dce"])
    sequences.append(["loadelim", "constfold", "copyprop", "cse", "copyprop", "dce"])
    sequences.append(["loadelim", "copyprop", "constfold", "cse", "copyprop", "dce"])
    sequences.append(ALL_PASSES * 2)

    seen = {tuple(s) for s in sequences}
    while len(sequences) < count:
        candidate = random_sequence(rng)
        key = tuple(candidate)
        if key not in seen:
            seen.add(key)
            sequences.append(candidate)

    return sequences[:count]


def _measure_program(args) -> dict | None:
    seed, size, max_depth, sequences = args
    tc = Toolchain.discover()

    source = generate_program(seed, size=size, max_depth=max_depth)

    with tempfile.TemporaryDirectory(prefix="mloptc_ds_") as tmp:
        path = pathlib.Path(tmp) / "program.tc"
        path.write_text(source, encoding="utf-8")

        if not tc.compiles(path):
            return None

        features = tc.features(path)
        if features is None:
            return None

        baseline = tc.measure(path, None)
        if baseline is None:
            return None

        # Programs that trap almost immediately carry almost no signal: their
        # dynamic cost is dominated by however far they got before the trap,
        # which no pass ordering can change much. Excluding them keeps the
        # dataset about optimization rather than about trapping.
        if baseline["dynamic_instructions"] < 50:
            return None

        samples = []
        for sequence in sequences:
            measured = tc.measure(path, sequence or None)
            if measured is None:
                continue
            # Every sequence must preserve behaviour. If one does not, that is a
            # compiler bug and the whole program is dropped rather than training
            # the model on a miscompilation.
            if (
                measured["output"] != baseline["output"]
                or measured["exit_status"] != baseline["exit_status"]
            ):
                return {"seed": seed, "source": source, "miscompile": True, "sequence": sequence}

            cost = max(1, measured["dynamic_instructions"])
            base = max(1, baseline["dynamic_instructions"])
            samples.append(
                {
                    "sequence": sequence,
                    "cost": cost,
                    "static_instructions": measured["static_instructions"],
                    "ratio": cost / base,
                    "log_ratio": math.log(cost / base),
                }
            )

        if not samples:
            return None

        return {
            "seed": seed,
            "features": features,
            "baseline": baseline["dynamic_instructions"],
            "baseline_static": baseline["static_instructions_before"],
            "samples": samples,
        }


def build_dataset(
    programs: int,
    sequences_count: int,
    *,
    seed: int = 0,
    size: str = "medium",
    max_depth: int = 3,
    jobs: int = 1,
) -> dict:
    rng = random.Random(seed)
    sequences = candidate_sequences(rng, sequences_count)

    tasks = [(seed + i, size, max_depth, sequences) for i in range(programs)]

    records = []
    miscompiles = []
    started = time.time()

    def consume(index: int, result) -> None:
        if result is None:
            return
        if result.get("miscompile"):
            miscompiles.append(result)
            print(f"\n!! MISCOMPILE on seed {result['seed']} with {result['sequence']}", file=sys.stderr)
            return
        records.append(result)

    if jobs > 1:
        with concurrent.futures.ProcessPoolExecutor(max_workers=jobs) as pool:
            for index, result in enumerate(pool.map(_measure_program, tasks), start=1):
                consume(index, result)
                if index % 10 == 0 or index == programs:
                    elapsed = time.time() - started
                    print(
                        f"\r{index}/{programs} programs | {len(records)} usable | {elapsed:.0f}s",
                        end="",
                        flush=True,
                    )
    else:
        for index, task in enumerate(tasks, start=1):
            consume(index, _measure_program(task))
            if index % 10 == 0 or index == programs:
                elapsed = time.time() - started
                print(
                    f"\r{index}/{programs} programs | {len(records)} usable | {elapsed:.0f}s",
                    end="",
                    flush=True,
                )

    print()
    return {
        "sequences": sequences,
        "records": records,
        "miscompiles": miscompiles,
        "generated_in_seconds": time.time() - started,
        "config": {
            "programs": programs,
            "sequences": sequences_count,
            "seed": seed,
            "size": size,
            "max_depth": max_depth,
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Build the pass-ordering dataset.")
    parser.add_argument("--programs", type=int, default=300)
    parser.add_argument("--sequences", type=int, default=24)
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument("--size", choices=["small", "medium", "large"], default="medium")
    parser.add_argument("--max-depth", type=int, default=3)
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument("--out", type=pathlib.Path, default=pathlib.Path("ml/data/dataset.json"))
    args = parser.parse_args()

    try:
        Toolchain.discover()
    except ToolchainError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    dataset = build_dataset(
        args.programs,
        args.sequences,
        seed=args.seed,
        size=args.size,
        max_depth=args.max_depth,
        jobs=args.jobs,
    )

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(dataset), encoding="utf-8")

    rows = sum(len(r["samples"]) for r in dataset["records"])
    print(f"\n{len(dataset['records'])} programs, {rows} (program, sequence) rows")
    print(f"written to {args.out}")

    if dataset["miscompiles"]:
        print(f"\n!! {len(dataset['miscompiles'])} MISCOMPILES found while building the dataset")
        print("   These are compiler bugs. Investigate before training.")
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
