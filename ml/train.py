"""Trains the pass-ordering model and exports it for the C++ compiler.

The model predicts, for one (program, pass-sequence) pair, the log of the
dynamic-instruction count relative to no optimization. At compile time the C++
side scores every candidate sequence and picks the lowest.

**The split is by PROGRAM, not by row.** Each program contributes one row per
candidate sequence, so splitting rows at random would put the same program on
both sides of the split, and the model could score well by memorising that
program rather than by learning anything about pass ordering. Grouping by
program is the difference between a believable held-out number and a
meaningless one.

**The headline metric is not R-squared.** What matters is: when the model picks
a sequence for an unseen program, how does that sequence actually perform
compared to the fixed -O2 pipeline? A model can have a mediocre R-squared and
still rank candidates well, which is all the compiler needs; and it can have a
good R-squared while picking badly. So evaluation measures the thing the
compiler does.

Usage:
    python ml/train.py --dataset ml/data/dataset.json --out ml/models/model.json
"""

from __future__ import annotations

import argparse
import json
import math
import pathlib
import sys

try:
    import numpy as np
    from sklearn.ensemble import GradientBoostingRegressor
    from sklearn.metrics import mean_absolute_error, r2_score
except ImportError:  # pragma: no cover
    print(
        "error: this script needs numpy and scikit-learn.\n"
        "  pip install -r ml/requirements.txt",
        file=sys.stderr,
    )
    raise SystemExit(2)

ALL_PASSES = ["loadelim", "constfold", "copyprop", "cse", "dce"]
MAX_SEQUENCE_LENGTH = 12


def encode_row(features: dict, feature_names: list[str], sequence: list[str]) -> list[float]:
    """Builds one model input.

    LAYOUT -- src/ml_scheduler.cpp reproduces this exactly, and the C++ side
    refuses to load a model whose feature_names do not match:

        [0 .. n_features)        the program's static IR features
        then per pass:           how many times it appears in the sequence
        then per position:       index of the pass there, or -1 if past the end

    The counts say WHICH passes run; the positions say in what ORDER. Counts
    alone would make every permutation of a sequence encode identically, which
    is precisely the distinction the model exists to make.
    """
    row = [float(features.get(name, 0.0)) for name in feature_names]
    row += [float(sequence.count(name)) for name in ALL_PASSES]
    for i in range(MAX_SEQUENCE_LENGTH):
        row.append(float(ALL_PASSES.index(sequence[i])) if i < len(sequence) else -1.0)
    return row


def build_matrix(records: list[dict], feature_names: list[str]):
    x_rows: list[list[float]] = []
    y_rows: list[float] = []
    groups: list[int] = []

    for index, record in enumerate(records):
        for sample in record["samples"]:
            x_rows.append(encode_row(record["features"], feature_names, sample["sequence"]))
            y_rows.append(sample["log_ratio"])
            groups.append(index)

    return np.array(x_rows, dtype=float), np.array(y_rows, dtype=float), np.array(groups)


def export_model(model: GradientBoostingRegressor, feature_names: list[str], candidates: list[list[str]]) -> dict:
    """Serialises the forest in the exact shape src/ml_scheduler.cpp expects.

    scikit-learn stores each tree as parallel arrays; we copy them straight
    across, so C++ inference is an array walk with no reconstruction step and
    no chance of the two sides disagreeing about tree structure.
    """
    trees = []
    for stage in model.estimators_:
        tree = stage[0].tree_
        trees.append(
            {
                "feature": [int(f) for f in tree.feature],
                "threshold": [float(t) for t in tree.threshold],
                "left": [int(c) for c in tree.children_left],
                "right": [int(c) for c in tree.children_right],
                "value": [float(v[0][0]) for v in tree.value],
            }
        )

    return {
        "format_version": 1,
        "feature_names": feature_names + [f"count_{p}" for p in ALL_PASSES]
        + [f"pos_{i}" for i in range(MAX_SEQUENCE_LENGTH)],
        "pass_vocabulary": ALL_PASSES,
        "max_sequence_length": MAX_SEQUENCE_LENGTH,
        "base_score": float(model.init_.constant_[0][0]),
        "learning_rate": float(model.learning_rate),
        "candidates": candidates,
        "trees": trees,
    }


def evaluate_selection(
    model: GradientBoostingRegressor,
    records: list[dict],
    feature_names: list[str],
    candidates: list[list[str]],
    o2_sequence: list[str],
) -> dict:
    """The metric that actually matters.

    For each held-out program: what the model picks, versus the fixed pipeline,
    versus the best sequence that existed (the oracle upper bound). Reporting
    the oracle alongside is what keeps the result honest -- it shows how much of
    the available win the model captured, rather than only whether it beat -O2.
    """
    o2_key = tuple(o2_sequence)

    model_costs: list[float] = []
    o2_costs: list[float] = []
    best_costs: list[float] = []
    wins = 0
    losses = 0
    ties = 0

    for record in records:
        by_sequence = {tuple(s["sequence"]): s for s in record["samples"]}
        if o2_key not in by_sequence:
            continue

        scored = []
        for candidate in candidates:
            key = tuple(candidate)
            if key not in by_sequence:
                continue
            x = np.array([encode_row(record["features"], feature_names, candidate)], dtype=float)
            scored.append((float(model.predict(x)[0]), key))

        if not scored:
            continue

        scored.sort()
        chosen = scored[0][1]

        model_ratio = by_sequence[chosen]["ratio"]
        o2_ratio = by_sequence[o2_key]["ratio"]
        best_ratio = min(s["ratio"] for s in record["samples"])

        model_costs.append(model_ratio)
        o2_costs.append(o2_ratio)
        best_costs.append(best_ratio)

        # A relative tolerance, because a difference of one instruction in ten
        # thousand is not a win in any meaningful sense.
        if model_ratio < o2_ratio * 0.999:
            wins += 1
        elif model_ratio > o2_ratio * 1.001:
            losses += 1
        else:
            ties += 1

    if not model_costs:
        return {"programs": 0}

    mean_model = sum(model_costs) / len(model_costs)
    mean_o2 = sum(o2_costs) / len(o2_costs)
    mean_best = sum(best_costs) / len(best_costs)

    return {
        "programs": len(model_costs),
        "mean_ratio_model": mean_model,
        "mean_ratio_o2": mean_o2,
        "mean_ratio_oracle_best": mean_best,
        # Positive means the model's choices run fewer instructions than -O2.
        "improvement_over_o2_percent": 100.0 * (mean_o2 - mean_model) / mean_o2 if mean_o2 else 0.0,
        # How much of the gap between -O2 and the best possible choice the
        # model closed. This is the number that says whether the model is
        # learning or merely copying -O2.
        "gap_closed_percent": (
            100.0 * (mean_o2 - mean_model) / (mean_o2 - mean_best) if mean_o2 > mean_best else 0.0
        ),
        "wins": wins,
        "losses": losses,
        "ties": ties,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Train the pass-ordering model.")
    parser.add_argument("--dataset", type=pathlib.Path, default=pathlib.Path("ml/data/dataset.json"))
    parser.add_argument("--out", type=pathlib.Path, default=pathlib.Path("ml/models/model.json"))
    parser.add_argument("--metrics", type=pathlib.Path, default=pathlib.Path("ml/models/metrics.json"))
    parser.add_argument("--test-fraction", type=float, default=0.25)
    parser.add_argument("--estimators", type=int, default=300)
    parser.add_argument("--max-depth", type=int, default=4)
    parser.add_argument("--learning-rate", type=float, default=0.06)
    parser.add_argument("--seed", type=int, default=0)
    args = parser.parse_args()

    if not args.dataset.is_file():
        print(f"error: no dataset at {args.dataset}. Run ml/dataset.py first.", file=sys.stderr)
        return 2

    dataset = json.loads(args.dataset.read_text(encoding="utf-8"))
    records = dataset["records"]
    candidates = dataset["sequences"]

    if len(records) < 10:
        print(f"error: only {len(records)} programs; generate a larger dataset.", file=sys.stderr)
        return 2

    # Feature names come from the dataset itself, so their ORDER matches what
    # the compiler emitted. The C++ loader checks it and refuses a mismatch.
    feature_names = list(records[0]["features"].keys())

    # --- split BY PROGRAM ----------------------------------------------------
    rng = np.random.RandomState(args.seed)
    order = rng.permutation(len(records))
    cut = int(len(records) * (1.0 - args.test_fraction))
    train_records = [records[i] for i in order[:cut]]
    test_records = [records[i] for i in order[cut:]]

    print(f"{len(records)} programs -> {len(train_records)} train / {len(test_records)} test")

    x_train, y_train, _ = build_matrix(train_records, feature_names)
    x_test, y_test, _ = build_matrix(test_records, feature_names)
    print(f"{x_train.shape[0]} training rows, {x_train.shape[1]} features")

    model = GradientBoostingRegressor(
        n_estimators=args.estimators,
        max_depth=args.max_depth,
        learning_rate=args.learning_rate,
        subsample=0.9,
        random_state=args.seed,
    )
    model.fit(x_train, y_train)

    predictions = model.predict(x_test)
    regression = {
        "r2": float(r2_score(y_test, predictions)),
        "mae_log_ratio": float(mean_absolute_error(y_test, predictions)),
    }
    print(f"\nregression on held-out rows: R2={regression['r2']:.3f} MAE={regression['mae_log_ratio']:.4f}")

    o2_sequence = next(
        (c for c in candidates if len(c) == 12 and c[0] == "loadelim"),
        candidates[1] if len(candidates) > 1 else candidates[0],
    )

    selection = evaluate_selection(model, test_records, feature_names, candidates, o2_sequence)

    print("\n--- what actually matters: pass selection on held-out programs ---")
    if selection.get("programs"):
        print(f"programs evaluated    : {selection['programs']}")
        print(f"mean ratio, -O2       : {selection['mean_ratio_o2']:.4f}")
        print(f"mean ratio, -O-ml     : {selection['mean_ratio_model']:.4f}")
        print(f"mean ratio, best known: {selection['mean_ratio_oracle_best']:.4f}")
        print(f"improvement over -O2  : {selection['improvement_over_o2_percent']:+.2f}%")
        print(f"gap to best closed    : {selection['gap_closed_percent']:.1f}%")
        print(f"wins/ties/losses      : {selection['wins']}/{selection['ties']}/{selection['losses']}")

        if selection["improvement_over_o2_percent"] <= 0:
            print(
                "\nNOTE: the model does NOT beat the fixed pipeline on this dataset.\n"
                "That is a real result and belongs in the README as-is."
            )
    else:
        print("not enough held-out data to evaluate selection")

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(export_model(model, feature_names, candidates)), encoding="utf-8")
    print(f"\nmodel written to {args.out}")

    args.metrics.parent.mkdir(parents=True, exist_ok=True)
    args.metrics.write_text(
        json.dumps(
            {
                "regression": regression,
                "selection": selection,
                "dataset": dataset.get("config", {}),
                "n_programs": len(records),
                "n_rows": int(x_train.shape[0] + x_test.shape[0]),
                "feature_importances": {
                    name: float(importance)
                    for name, importance in sorted(
                        zip(
                            feature_names
                            + [f"count_{p}" for p in ALL_PASSES]
                            + [f"pos_{i}" for i in range(MAX_SEQUENCE_LENGTH)],
                            model.feature_importances_,
                        ),
                        key=lambda pair: -pair[1],
                    )[:15]
                },
            },
            indent=2,
        ),
        encoding="utf-8",
    )
    print(f"metrics written to {args.metrics}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
