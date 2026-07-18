#!/usr/bin/env python3
"""Render the Day-6 benchmark charts into docs/img/ (PLAN.md [docs: plots]).

Two subcommands, one chart each:

  latency --csv FILE --out FILE.svg
      Per-op-type latency percentile chart from `lob_e2e_bench FILE --csv`
      output (op,count,min_ns,p50_ns,p90_ns,p99_ns,p999_ns,max_ns).

  compare --json FILE --out FILE.svg
      Naive-vs-optimized throughput bars from a Google Benchmark JSON dump
      (--benchmark_out=FILE --benchmark_out_format=json). Data-driven: every
      "<Family>/naive/<range>" + "<Family>/book/<range>" pair found in the
      file becomes one bar group, so the bench filter controls chart content.
      Aggregate files (--benchmark_repetitions) use the mean rows.

Charts are SVG (crisp in the README, reviewable in git); output is
deterministic (fixed hashsalt, no embedded date) so regeneration diffs
cleanly. Exit codes match the repo's CLI tools: 0 ok, 1 bad input data,
2 usage.
"""

import argparse
import csv
import json
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")  # headless; never touch a display
import matplotlib.pyplot as plt  # noqa: E402  (backend must be set first)

# Deterministic SVG output: stable element ids, no creation date.
plt.rcParams["svg.hashsalt"] = "lob-plot-bench"
SVG_METADATA = {"Date": None}

NAIVE_COLOR = "#9aa0a6"  # gray: the std::map baseline
BOOK_COLOR = "#1a73e8"  # blue: the banded-array book
OP_COLORS = {
    "limit": "#1a73e8",
    "market": "#d93025",
    "ioc": "#188038",
    "cancel": "#f9ab00",
    "all": "#202124",
}

PERCENTILE_COLUMNS = [
    ("p50_ns", "p50"),
    ("p90_ns", "p90"),
    ("p99_ns", "p99"),
    ("p999_ns", "p99.9"),
    ("max_ns", "max"),
]


def si_format(value: float) -> str:
    """1_234_000 -> '1.2M' (axis and annotation labels)."""
    for scale, suffix in ((1e9, "G"), (1e6, "M"), (1e3, "k")):
        if value >= scale:
            return f"{value / scale:.3g}{suffix}"
    return f"{value:.3g}"


def new_axes(title: str):
    fig, ax = plt.subplots(figsize=(8.0, 4.5), layout="constrained")
    ax.set_title(title)
    ax.grid(True, axis="y", which="major", alpha=0.3)
    ax.spines[["top", "right"]].set_visible(False)
    return fig, ax


def save(fig, out_path: Path) -> None:
    out_path.parent.mkdir(parents=True, exist_ok=True)
    if out_path.suffix.lower() == ".svg":
        # Deterministic metadata is an SVG concept; PNG et al. take a dpi.
        fig.savefig(out_path, format="svg", metadata=SVG_METADATA)
    else:
        fig.savefig(out_path, dpi=160)
    plt.close(fig)
    print(f"wrote {out_path}")


def plot_latency(args) -> int:
    try:
        with open(args.csv, newline="", encoding="utf-8") as f:
            rows = list(csv.DictReader(f))
    except OSError as err:
        print(f"error: cannot read '{args.csv}': {err}", file=sys.stderr)
        return 1
    rows = [r for r in rows if r.get("op") and int(r.get("count", 0) or 0) > 0]
    if not rows:
        print(f"error: no latency rows in '{args.csv}'", file=sys.stderr)
        return 1

    fig, ax = new_axes(args.title)
    xs = range(len(PERCENTILE_COLUMNS))
    for row in rows:
        op = row["op"]
        ys = [int(row[col]) for col, _ in PERCENTILE_COLUMNS]
        is_all = op == "all"
        ax.plot(
            xs,
            ys,
            marker="o",
            markersize=4,
            linewidth=2.2 if is_all else 1.6,
            linestyle="--" if is_all else "-",
            color=OP_COLORS.get(op, "#5f6368"),
            label=f"{op} (n={si_format(int(row['count']))})",
        )
    ax.set_yscale("log")
    ax.set_xticks(list(xs), [label for _, label in PERCENTILE_COLUMNS])
    ax.set_ylabel("latency, ns (log scale)")
    ax.legend(frameon=False, fontsize=9)
    save(fig, Path(args.out))
    return 0


def load_throughputs(path: str) -> dict[str, float]:
    """Benchmark name -> items_per_second; mean rows win in aggregate files."""
    with open(path, encoding="utf-8") as f:
        doc = json.load(f)
    result: dict[str, float] = {}
    for row in doc.get("benchmarks", []):
        if "items_per_second" not in row:
            continue
        if row.get("run_type") == "aggregate":
            if row.get("aggregate_name") != "mean":
                continue
            name = row.get("run_name", "")
        else:
            name = row.get("name", "")
        if name:
            result[name] = float(row["items_per_second"])
    return result


def pair_up(throughputs: dict[str, float]) -> list[tuple[str, float, float]]:
    """[(label, naive, book)] for every family/range present as both."""
    pairs = []
    for name, naive_ops in throughputs.items():
        if "/naive" not in name:
            continue
        book_name = name.replace("/naive", "/book", 1)
        if book_name not in throughputs:
            continue
        family, _, rest = name.partition("/naive")
        # Two-line tick label ("MatchSteady\n4096") so groups never collide.
        label = family + "\n" + rest.lstrip("/") if rest else family
        pairs.append((label, naive_ops, throughputs[book_name]))
    return pairs


def plot_compare(args) -> int:
    try:
        throughputs = load_throughputs(args.json)
    except (OSError, json.JSONDecodeError) as err:
        print(f"error: cannot load '{args.json}': {err}", file=sys.stderr)
        return 1
    pairs = pair_up(throughputs)
    if not pairs:
        print(f"error: no naive/book benchmark pairs in '{args.json}'", file=sys.stderr)
        return 1

    fig, ax = new_axes(args.title)
    xs = range(len(pairs))
    width = 0.38
    naive_ys = [naive for _, naive, _ in pairs]
    book_ys = [book for _, _, book in pairs]
    ax.bar([x - width / 2 for x in xs], naive_ys, width, label="NaiveBook (std::map)",
           color=NAIVE_COLOR)
    bars = ax.bar([x + width / 2 for x in xs], book_ys, width, label="OrderBook (banded array)",
                  color=BOOK_COLOR)
    for bar, (_, naive_ops, book_ops) in zip(bars, pairs):
        ax.annotate(
            f"{book_ops / naive_ops:.1f}×",
            (bar.get_x() + bar.get_width() / 2, bar.get_height()),
            textcoords="offset points",
            xytext=(0, 3),
            ha="center",
            fontsize=9,
            fontweight="bold",
        )
    ax.set_yscale("log")
    ax.set_xticks(list(xs), [label for label, _, _ in pairs], fontsize=9)
    ax.set_ylabel("throughput, ops/s (log scale)")
    ax.yaxis.set_major_formatter(lambda v, _pos: si_format(v))
    ax.yaxis.set_minor_formatter(lambda v, _pos: si_format(v))
    ax.tick_params(axis="y", which="minor", labelsize=8)
    # Headroom so the ratio annotations never clip at the top edge.
    low, high = ax.get_ylim()
    ax.set_ylim(low, high * 1.35)
    ax.legend(frameon=False, fontsize=9)
    save(fig, Path(args.out))
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)

    latency = sub.add_parser("latency", help="percentile chart from lob_e2e_bench --csv output")
    latency.add_argument("--csv", required=True, help="lob_e2e_bench --csv output file")
    latency.add_argument("--out", required=True, help="output SVG path")
    latency.add_argument("--title", default="End-to-end latency percentiles by op type")
    latency.set_defaults(func=plot_latency)

    compare = sub.add_parser("compare", help="naive-vs-book bars from Google Benchmark JSON")
    compare.add_argument("--json", required=True, help="--benchmark_out JSON file")
    compare.add_argument("--out", required=True, help="output SVG path")
    compare.add_argument("--title", default="NaiveBook (std::map) vs OrderBook (banded array)")
    compare.set_defaults(func=plot_compare)

    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
