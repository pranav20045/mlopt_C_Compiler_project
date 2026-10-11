"""Renders the benchmark charts for the README.

Design notes, since charts are read by people and this one has a job to do:

  * The measure is a RATIO of dynamic instructions to unoptimized, so every
    benchmark is on the same scale and the chart is readable without the reader
    holding absolute counts in their head. Lower is better, and a reference line
    at 1.0 marks "no improvement".
  * Horizontal bars, because benchmark names are words and horizontal bars give
    them room without rotated labels.
  * One x-axis only. Where two measures of different scale appear, they get
    their own figure rather than a second y-axis.
  * Colors are assigned by SERIES IDENTITY and stay fixed, so -O2 is the same
    hue in every chart.

The palette is the validated three-slot categorical set (blue / orange / aqua),
which clears the colourblind-separation and normal-vision floors for all pairs.
Series are also direct-labeled with their values, so identity is never carried
by colour alone.
"""

from __future__ import annotations

import argparse
import json
import math
import pathlib
import sys

try:
    import matplotlib

    matplotlib.use("Agg")  # no display needed; CI has none
    import matplotlib.pyplot as plt
    from matplotlib.ticker import FuncFormatter
except ImportError:  # pragma: no cover
    print("error: matplotlib is required.  pip install -r ml/requirements.txt", file=sys.stderr)
    raise SystemExit(2)

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent

# --- the validated palette ----------------------------------------------------
SURFACE = "#fcfcfb"
TEXT_PRIMARY = "#0b0b0b"
TEXT_SECONDARY = "#52514e"
GRID = "#e4e3df"

SERIES = {
    "O0": "#52514e",      # neutral: the baseline is a reference, not a competitor
    "O2": "#2a78d6",      # slot 1, blue
    "O-ml": "#eb6834",    # slot 2, orange
    "best": "#1baf7a",    # slot 3, aqua
}

LABELS = {
    "O0": "unoptimized",
    "O2": "-O2 (fixed pipeline)",
    "O-ml": "-O-ml (learned)",
    "best": "best known sequence",
}


def _style_axes(ax, *, xlabel: str) -> None:
    ax.set_facecolor(SURFACE)
    ax.set_xlabel(xlabel, color=TEXT_SECONDARY, fontsize=10)
    ax.tick_params(colors=TEXT_SECONDARY, labelsize=9, length=0)
    # Recessive grid: visible enough to read against, never competing with data.
    ax.grid(axis="x", color=GRID, linewidth=0.8, zorder=0)
    ax.set_axisbelow(True)
    for side in ("top", "right", "left", "bottom"):
        ax.spines[side].set_visible(False)
    for label in ax.get_yticklabels():
        label.set_color(TEXT_PRIMARY)


def chart_relative_cost(results: dict, out_path: pathlib.Path) -> None:
    """Per-benchmark dynamic instructions, relative to unoptimized."""
    records = [r for r in results["benchmarks"] if "O2" in r["levels"]]
    if not records:
        print("no -O2 results to chart", file=sys.stderr)
        return

    has_ml = any("O-ml" in r["levels"] for r in records)
    series = ["O2"] + (["O-ml"] if has_ml else [])

    # Sorted by -O2 ratio so the chart tells a story: where the optimizer helps
    # most is immediately visible.
    records.sort(key=lambda r: r["levels"]["O2"]["ratio"])
    names = [r["name"] for r in records]

    fig, ax = plt.subplots(figsize=(9, 0.62 * len(records) + 2.0))
    fig.patch.set_facecolor(SURFACE)

    bar_height = 0.34 if len(series) > 1 else 0.5
    positions = range(len(records))

    for index, key in enumerate(series):
        offset = (index - (len(series) - 1) / 2) * (bar_height + 0.04)
        values = [r["levels"].get(key, {}).get("ratio", float("nan")) for r in records]
        bars = ax.barh(
            [p - offset for p in positions],
            values,
            height=bar_height,
            color=SERIES[key],
            label=LABELS[key],
            zorder=3,
        )
        # Selective direct labels: every bar, because there are few of them and
        # the exact ratio is the point.
        for bar, value in zip(bars, values):
            if value == value:  # not NaN
                ax.text(
                    value + 0.012,
                    bar.get_y() + bar.get_height() / 2,
                    f"{value:.2f}",
                    va="center",
                    fontsize=8,
                    color=TEXT_SECONDARY,
                )

    # The "no improvement" reference. Dashed and recessive: it is an annotation,
    # not a series.
    ax.axvline(1.0, color=TEXT_SECONDARY, linewidth=1.0, linestyle="--", zorder=2)
    ax.text(1.0, len(records) - 0.3, "  unoptimized", fontsize=8, color=TEXT_SECONDARY, va="bottom")

    ax.set_yticks(list(positions))
    ax.set_yticklabels(names)
    ax.set_xlim(0, max(1.12, max(r["levels"]["O2"]["ratio"] for r in records) * 1.22))
    _style_axes(ax, xlabel="dynamic instructions, relative to unoptimized (lower is better)")

    ax.set_title(
        "Optimization effect per benchmark",
        color=TEXT_PRIMARY,
        fontsize=13,
        loc="left",
        pad=16,
        fontweight="bold",
    )

    legend = ax.legend(loc="lower right", frameon=False, fontsize=9)
    for text in legend.get_texts():
        text.set_color(TEXT_SECONDARY)

    fig.tight_layout()
    out_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_path, dpi=160, facecolor=SURFACE)
    plt.close(fig)
    print(f"wrote {out_path}")


def chart_pass_contribution(results: dict, out_path: pathlib.Path) -> None:
    """How much each pass achieves ON ITS OWN, averaged over the suite.

    A single series, so no legend: the title names it. This chart is the
    evidence for the claim that loadelim is the pass that matters -- everything
    else is nearly a no-op without it, because in a non-SSA IR the other passes
    cannot see through loads and stores.
    """
    pass_names = ["loadelim", "constfold", "copyprop", "cse", "dce"]
    rows: list[tuple[str, float]] = []

    for name in pass_names:
        key = f"only-{name}"
        ratios = [r["levels"][key]["ratio"] for r in results["benchmarks"] if key in r["levels"]]
        if ratios:
            geo = math.exp(sum(math.log(x) for x in ratios) / len(ratios))
            rows.append((name, geo))

    o2_ratios = [r["levels"]["O2"]["ratio"] for r in results["benchmarks"] if "O2" in r["levels"]]
    if o2_ratios:
        rows.append(("-O2 (all, twice)", math.exp(sum(math.log(x) for x in o2_ratios) / len(o2_ratios))))

    if not rows:
        return

    rows.sort(key=lambda pair: -pair[1])
    names = [r[0] for r in rows]
    values = [r[1] for r in rows]

    fig, ax = plt.subplots(figsize=(9, 0.55 * len(rows) + 2.0))
    fig.patch.set_facecolor(SURFACE)

    # The combined pipeline is a different kind of thing from a single pass, so
    # it is coloured differently rather than blending into the series.
    colors = [SERIES["O2"] if n.startswith("-O2") else SERIES["best"] for n in names]

    bars = ax.barh(range(len(rows)), values, height=0.52, color=colors, zorder=3)
    for bar, value in zip(bars, values):
        ax.text(
            value + 0.008,
            bar.get_y() + bar.get_height() / 2,
            f"{value:.3f}",
            va="center",
            fontsize=8,
            color=TEXT_SECONDARY,
        )

    ax.axvline(1.0, color=TEXT_SECONDARY, linewidth=1.0, linestyle="--", zorder=2)

    ax.set_yticks(range(len(rows)))
    ax.set_yticklabels(names)
    ax.set_xlim(0, 1.15)
    ax.xaxis.set_major_formatter(FuncFormatter(lambda v, _: f"{v:.2f}"))
    _style_axes(ax, xlabel="geometric mean ratio vs unoptimized (lower is better)")

    ax.set_title(
        "What each pass achieves on its own",
        color=TEXT_PRIMARY,
        fontsize=13,
        loc="left",
        pad=16,
        fontweight="bold",
    )

    fig.tight_layout()
    out_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_path, dpi=160, facecolor=SURFACE)
    plt.close(fig)
    print(f"wrote {out_path}")


def write_markdown_table(results: dict, out_path: pathlib.Path) -> None:
    """A table view of the same numbers.

    Not decoration: a chart that carries information only visually is not
    accessible, and the README quotes these figures directly.
    """
    records = [r for r in results["benchmarks"] if "O2" in r["levels"]]
    records.sort(key=lambda r: r["name"])
    has_ml = any("O-ml" in r["levels"] for r in records)

    lines = [
        "| benchmark | unoptimized | -O2 | ratio |" + (" -O-ml | ratio |" if has_ml else ""),
        "|---|---:|---:|---:|" + ("---:|---:|" if has_ml else ""),
    ]
    for record in records:
        o2 = record["levels"]["O2"]
        row = (
            f"| {record['name']} | {record['baseline_dynamic']:,} "
            f"| {o2['dynamic']:,} | {o2['ratio']:.3f} |"
        )
        if has_ml:
            ml = record["levels"].get("O-ml")
            row += f" {ml['dynamic']:,} | {ml['ratio']:.3f} |" if ml else " - | - |"
        lines.append(row)

    for label in ("O2", "O-ml"):
        ratios = [r["levels"][label]["ratio"] for r in records if label in r["levels"]]
        if ratios:
            geo = math.exp(sum(math.log(x) for x in ratios) / len(ratios))
            lines.append(f"\n**Geometric mean ratio, -{label}: {geo:.4f}** ({100 * (1 - geo):+.1f}% instructions)")

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"wrote {out_path}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Render the benchmark charts.")
    parser.add_argument(
        "--results", type=pathlib.Path, default=REPO_ROOT / "bench" / "results" / "results.json"
    )
    parser.add_argument("--outdir", type=pathlib.Path, default=REPO_ROOT / "bench" / "results")
    args = parser.parse_args()

    if not args.results.is_file():
        print(f"error: no results at {args.results}. Run bench/run_benchmarks.py first.", file=sys.stderr)
        return 2

    results = json.loads(args.results.read_text(encoding="utf-8"))

    chart_relative_cost(results, args.outdir / "relative_cost.png")
    chart_pass_contribution(results, args.outdir / "pass_contribution.png")
    write_markdown_table(results, args.outdir / "results.md")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
