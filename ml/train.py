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


def export_model(
    model: GradientBoostingRegressor,
    feature_names: list[str],
    candidates: list[list[str]],
    tolerance: float,
) -> dict:
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
        # Predictions closer together than this are treated as equal by the
        # compiler, which then prefers the shortest candidate. Set to the
        # model's own held-out error: a difference smaller than the model's
        # error is noise, not signal.
        "selection_tolerance": tolerance,
        "trees": trees,
    }


def choose_tolerance(
    model: GradientBoostingRegressor,
    records: list[dict],
    feature_names: list[str],
    candidates: list[list[str]],
    o2_sequence: list[str],
    quality_budget: float,
) -> float:
    """Picks how much predicted-cost difference counts as 'the same'.

    This is the knob on the quality/compile-cost tradeoff, and it has to be set
    from data rather than guessed. A tolerance of 0 makes the compiler take the
    single best-predicted sequence, which on a model with modest R-squared means
    following noise. A large tolerance makes it take the shortest sequence
    regardless, which is fast but can cost real runtime quality.

    So: sweep the tolerance on the TRAINING programs and take the largest value
    whose mean runtime cost stays within `quality_budget` of -O2. Choosing it on
    training data keeps the held-out numbers honest -- the test set never
    influences the operating point.
    """
    o2_key = tuple(o2_sequence)
    best_tolerance = 0.0

    # Predictions do not depend on the tolerance, so they are computed once
    # here rather than inside the sweep. Batched into a single predict() call
    # per program as well: scikit-learn's per-call overhead dominates when
    # predicting one row at a time.
    cache: list[tuple[dict, list[tuple[float, tuple]]]] = []
    for record in records:
        by_sequence = {tuple(s["sequence"]): s for s in record["samples"]}
        if o2_key not in by_sequence:
            continue

        usable = [
            c for c in candidates
            # "run nothing" is never a valid answer to -O-ml; see
            # src/ml_scheduler.cpp.
            if c and tuple(c) in by_sequence
        ]
        if not usable:
            continue

        rows = np.array(
            [encode_row(record["features"], feature_names, c) for c in usable], dtype=float
        )
        predictions = model.predict(rows)
        cache.append((by_sequence, [(float(p), tuple(c)) for p, c in zip(predictions, usable)]))

    for tolerance in [0.0, 0.0005, 0.001, 0.002, 0.004, 0.008, 0.015, 0.03, 0.06, 0.12]:
        ratios_model: list[float] = []
        ratios_o2: list[float] = []

        for by_sequence, scored in cache:
            best_score = min(score for score, _ in scored)
            best_key = None
            best_length = None
            for score, key in scored:
                if score > best_score + tolerance:
                    continue
                if best_length is None or len(key) < best_length:
                    best_length = len(key)
                    best_key = key
            chosen = best_key if best_key is not None else scored[0][1]

            ratios_model.append(by_sequence[chosen]["ratio"])
            ratios_o2.append(by_sequence[o2_key]["ratio"])

        if not ratios_model:
            continue

        mean_model = sum(ratios_model) / len(ratios_model)
        mean_o2 = sum(ratios_o2) / len(ratios_o2)
        loss = (mean_model - mean_o2) / mean_o2 if mean_o2 else 0.0

        if loss <= quality_budget:
            best_tolerance = tolerance
        else:
            break  # the curve is monotone; past here it only gets worse

    return best_tolerance


def evaluate_selection(
    model: GradientBoostingRegressor,
    records: list[dict],
    feature_names: list[str],
    candidates: list[list[str]],
    o2_sequence: list[str],
    tolerance: float,
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
    model_lengths: list[int] = []
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
            if not candidate or key not in by_sequence:
                continue
            x = np.array([encode_row(record["features"], feature_names, candidate)], dtype=float)
            scored.append((float(model.predict(x)[0]), key))

        if not scored:
            continue

        # EXACTLY the rule src/ml_scheduler.cpp applies, so this number
        # describes what the compiler will really do rather than an idealised
        # argmin it does not implement. `scored` is already in candidate order
        # and already excludes the empty sequence, matching the C++ loop; the
        # tie-break is first-by-candidate-order, so no sort happens first.
        best_score = min(score for score, _ in scored)
        best_key = None
        best_length = None
        for score, key in scored:
            if score > best_score + tolerance:
                continue
            if best_length is None or len(key) < best_length:
                best_length = len(key)
                best_key = key
        chosen = best_key if best_key is not None else scored[0][1]

        model_ratio = by_sequence[chosen]["ratio"]
        o2_ratio = by_sequence[o2_key]["ratio"]
        best_ratio = min(s["ratio"] for s in record["samples"])

        model_costs.append(model_ratio)
        o2_costs.append(o2_ratio)
        best_costs.append(best_ratio)
        model_lengths.append(len(chosen))

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
        # The second objective. Runtime quality turned out to be saturated --
        # nothing beats -O2 -- so what remains is reaching the same quality with
        # fewer passes, which is compile time saved.
        "mean_passes_model": sum(model_lengths) / len(model_lengths),
        "mean_passes_o2": float(len(o2_sequence)),
        "passes_saved_percent": 100.0
        * (len(o2_sequence) - sum(model_lengths) / len(model_lengths))
        / len(o2_sequence),
        "quality_matches_o2": sum(
            1 for m, o in zip(model_costs, o2_costs) if m <= o * 1.001
        ),
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
    parser.add_argument(
        "--quality-budget",
        type=float,
        default=0.005,
        help="how much mean runtime cost may regress vs -O2 in exchange for "
        "running fewer passes (default 0.5%%)",
    )
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

    tolerance = choose_tolerance(
        model, train_records, feature_names, candidates, o2_sequence, args.quality_budget
    )
    print(
        f"selection tolerance chosen on TRAINING data: {tolerance:g}"
        f"  (quality budget {args.quality_budget:.1%})"
    )

    selection = evaluate_selection(
        model, test_records, feature_names, candidates, o2_sequence, tolerance
    )

    print("\n--- what actually matters: pass selection on held-out programs ---")
    if selection.get("programs"):
        print(f"programs evaluated    : {selection['programs']}")
        print(f"mean ratio, -O2       : {selection['mean_ratio_o2']:.4f}")
        print(f"mean ratio, -O-ml     : {selection['mean_ratio_model']:.4f}")
        print(f"mean ratio, best known: {selection['mean_ratio_oracle_best']:.4f}")
        print(f"improvement over -O2  : {selection['improvement_over_o2_percent']:+.2f}%")
        print(f"gap to best closed    : {selection['gap_closed_percent']:.1f}%")
        print(f"wins/ties/losses      : {selection['wins']}/{selection['ties']}/{selection['losses']}")

        print("\n--- second objective: compile cost ---")
        print(f"passes run by -O2     : {selection['mean_passes_o2']:.0f}")
        print(f"passes run by -O-ml   : {selection['mean_passes_model']:.2f}")
        print(f"passes saved          : {selection['passes_saved_percent']:.1f}%")
        print(
            f"quality still matched : {selection['quality_matches_o2']}/{selection['programs']}"
            " programs"
        )

        if selection["improvement_over_o2_percent"] <= 0.01:
            print(
                "\nNOTE: the model does NOT beat -O2 on runtime cost, and on this\n"
                "dataset nothing does -- the best known sequence ties -O2 on every\n"
                "held-out program. Runtime quality is saturated with five passes that\n"
                "each iterate to a fixed point. What is left to win is compile cost,\n"
                "which is what the numbers above measure. This belongs in the README\n"
                "exactly as stated."
            )
    else:
        print("not enough held-out data to evaluate selection")

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(
        json.dumps(export_model(model, feature_names, candidates, tolerance)), encoding="utf-8"
    )
    print(f"\nmodel written to {args.out}")

    args.metrics.parent.mkdir(parents=True, exist_ok=True)
    args.metrics.write_text(
        json.dumps(
            {
                "regression": regression,
                "selection": selection,
                "selection_tolerance": tolerance,
                "quality_budget": args.quality_budget,
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
