"""Merges several dataset files into one.

Why this exists: the first model was trained only on small random programs and
generalised poorly to the hand-written benchmarks, which are larger and far more
loop-heavy. On `binary_search` and `fib_recursive` it selected a pipeline
without `loadelim` and delivered zero improvement.

That is textbook distribution shift -- the model was fine on data like its
training set and wrong off it -- and the fix is to widen the training
distribution rather than to special-case the benchmarks. This script merges
datasets generated at different sizes and depths.

Requires every input to share the same candidate sequence list, since the model
learns to rank exactly that set.

Usage:
    python ml/merge_datasets.py ml/data/dataset.json ml/data/dataset_large.json \\
        --out ml/data/merged.json
"""

from __future__ import annotations

import argparse
import json
import pathlib
import sys


def main() -> int:
    parser = argparse.ArgumentParser(description="Merge MLOpt-C datasets.")
    parser.add_argument("inputs", nargs="+", type=pathlib.Path)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    args = parser.parse_args()

    merged: dict | None = None
    total_miscompiles = 0

    for path in args.inputs:
        if not path.is_file():
            print(f"error: {path} does not exist", file=sys.stderr)
            return 2

        data = json.loads(path.read_text(encoding="utf-8"))
        total_miscompiles += len(data.get("miscompiles", []))

        if merged is None:
            merged = data
            print(f"{path.name}: {len(data['records'])} programs (base)")
            continue

        # The candidate set defines the model's action space. Mixing datasets
        # with different candidates would silently train on rows the compiler
        # can never choose.
        if data["sequences"] != merged["sequences"]:
            print(
                f"error: {path.name} has a different candidate sequence list.\n"
                "       Regenerate both with the same --sequences value.",
                file=sys.stderr,
            )
            return 1

        merged["records"].extend(data["records"])
        print(f"{path.name}: {len(data['records'])} programs (merged)")

    assert merged is not None
    merged["miscompiles"] = []
    merged["config"] = {"merged_from": [str(p) for p in args.inputs]}

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(merged), encoding="utf-8")

    rows = sum(len(r["samples"]) for r in merged["records"])
    print(f"\n{len(merged['records'])} programs, {rows} rows -> {args.out}")

    if total_miscompiles:
        print(f"!! {total_miscompiles} miscompiles were recorded across the inputs")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
