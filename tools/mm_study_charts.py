"""Charts for the Step 4 market-making study (PROGRESS.md, Rule P).

    uv run --project python --extra analysis python tools/mm_study_charts.py results/mm_study

Reads the CSV files written by tools/mm_study and writes PNG charts next to
them. It only plots the measured numbers in those files.
"""

import csv
import sys
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

SYMBOLS = ["AAPL", "MSFT", "INTC", "COLB"]
STRATEGIES = [("symmetric", "Symmetric"), ("avellaneda_stoikov", "Avellaneda-Stoikov")]
MODELS = [("naive", "naive (filled on touch)"), ("queue_aware", "queue-aware")]
LAT = [0, 10_000, 100_000, 1_000_000, 10_000_000]
LAT_LABEL = ["0", "10 µs", "100 µs", "1 ms", "10 ms"]
HOR = [0, 1_000_000, 10_000_000, 100_000_000, 1_000_000_000, 10_000_000_000, 60_000_000_000]
HOR_LABEL = ["0", "1 ms", "10 ms", "100 ms", "1 s", "10 s", "60 s"]

# Reference categorical palette (dataviz skill), fixed order; text never wears it.
C1, C2 = "#2a78d6", "#eb6834"
SURFACE, INK, INK2, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#e4e3df"


def style(ax, title):
    ax.set_facecolor(SURFACE)
    ax.set_title(title, fontsize=10, color=INK, loc="left")
    ax.grid(axis="y", color=GRID, linewidth=0.8)
    ax.axhline(0, color=INK2, linewidth=0.8)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    for s in ("left", "bottom"):
        ax.spines[s].set_color(GRID)
    ax.tick_params(colors=INK2, labelsize=8)


def line(ax, xs, ys, color, label):
    ax.plot(xs, ys, color=color, linewidth=2, marker="o", markersize=5, label=label,
            markeredgecolor=SURFACE, markeredgewidth=1.5)


def read(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def num(v):
    return float(v) if v not in ("", None) else float("nan")


def main(out: Path) -> None:
    runs = read(out / "latency_curve.csv")
    mk = read(out / "markouts.csv")
    inv = read(out / "invented_profit.csv")

    by = {(r["strategy"], r["symbol"], r["fill_model"], int(r["latency_ns"])): r for r in runs}

    # 1. Latency curve, queue-aware: P&L and fill rate (one panel per symbol and metric).
    fig, axes = plt.subplots(4, 2, figsize=(10, 12), facecolor=SURFACE)
    for i, sym in enumerate(SYMBOLS):
        for j, (metric, title, scale) in enumerate([("pnl_usd", "P&L at 15:55 ($)", 1.0),
                                                     ("fill_rate", "fill rate (%)", 100.0)]):
            ax = axes[i][j]
            style(ax, f"{sym} — {title}")
            ends = []
            for (st, name), c in zip(STRATEGIES, (C1, C2)):
                ys = [num(by[(st, sym, "queue_aware", l)][metric]) * scale for l in LAT]
                line(ax, range(len(LAT)), ys, c, name)
                ends.append((ys[-1], name))
            # Direct end labels, nudged apart when the two lines end close together.
            lo, hi = ax.get_ylim()
            close = abs(ends[0][0] - ends[1][0]) < 0.06 * (hi - lo)
            for y, name in ends:
                dy = 0 if not close else (6 if y >= max(e[0] for e in ends) else -6)
                ax.annotate(name, (len(LAT) - 1, y), xytext=(6, dy), textcoords="offset points",
                            fontsize=7, color=INK2, va="center")
            ax.set_xticks(range(len(LAT)), LAT_LABEL)
            ax.set_xlim(-0.3, len(LAT) + 0.9)
    axes[0][0].legend(fontsize=8, frameon=False, loc="best")
    fig.suptitle("Queue-aware fills: P&L and fill rate against one-way latency (both directions)",
                 fontsize=11, color=INK, x=0.02, ha="left")
    fig.supxlabel("one-way latency", fontsize=9, color=INK2)
    fig.tight_layout()
    fig.savefig(out / "latency_curve.png", dpi=130)
    plt.close(fig)

    # 2. Markout curves at latency 0 (two versions: mid move, and vs the fill price).
    m = defaultdict(dict)
    for r in mk:
        if int(r["latency_ns"]) != 0:
            continue
        m[(r["strategy"], r["symbol"], r["fill_model"])][int(r["horizon_ns"])] = r
    for col, fname, what in [("markout_mid_cents", "markout_curves.png",
                              "mid move: side × (mid after h − mid just before the fill)"),
                             ("markout_vs_fill_cents", "markout_curves_vs_fill.png",
                              "vs fill price: side × (mid after h − fill price)")]:
        fig, axes = plt.subplots(2, 4, figsize=(14, 6.5), facecolor=SURFACE, sharex=True)
        for i, (st, sname) in enumerate(STRATEGIES):
            for j, sym in enumerate(SYMBOLS):
                ax = axes[i][j]
                style(ax, f"{sname} — {sym}")
                for (model, mname), c in zip(MODELS, (C1, C2)):
                    ys = [num(m[(st, sym, model)][h][col]) for h in HOR]
                    line(ax, range(len(HOR)), ys, c, mname)
                ax.set_xticks(range(len(HOR)), HOR_LABEL, rotation=45)
                if j == 0:
                    ax.set_ylabel("cents per share", fontsize=8, color=INK2)
        axes[0][0].legend(fontsize=8, frameon=False, loc="best")
        fig.suptitle(f"Markout curves at latency 0, share-weighted ({what}); below 0 = adverse selection",
                     fontsize=11, color=INK, x=0.01, ha="left")
        fig.supxlabel("horizon after the fill", fontsize=9, color=INK2)
        fig.tight_layout()
        fig.savefig(out / fname, dpi=130)
        plt.close(fig)

    # 3. Invented profit against latency.
    ip = {(r["strategy"], r["symbol"], int(r["latency_ns"])): num(r["invented_profit_usd"]) for r in inv}
    fig, axes = plt.subplots(1, 4, figsize=(14, 3.8), facecolor=SURFACE)
    for j, sym in enumerate(SYMBOLS):
        ax = axes[j]
        style(ax, sym)
        for (st, name), c in zip(STRATEGIES, (C1, C2)):
            line(ax, range(len(LAT)), [ip[(st, sym, l)] for l in LAT], c, name)
        ax.set_xticks(range(len(LAT)), LAT_LABEL)
        if j == 0:
            ax.set_ylabel("$", fontsize=8, color=INK2)
    axes[0].legend(fontsize=8, frameon=False, loc="best")
    fig.suptitle("Invented profit = P&L(naive) − P&L(queue-aware), against one-way latency",
                 fontsize=11, color=INK, x=0.01, ha="left")
    fig.tight_layout()
    fig.savefig(out / "invented_profit.png", dpi=130)
    plt.close(fig)
    print(f"wrote charts to {out}")


if __name__ == "__main__":
    main(Path(sys.argv[1]))
