#!/usr/bin/env python3
import argparse
import csv
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

try:
    import seaborn as sns
    sns.set_theme(style="whitegrid")
except Exception:
    sns = None


COLORS = {
    "baseline": "#4C78A8",
    "observe": "#F58518",
    "intervene": "#54A24B",
    "ms": "#F58518",
    "degrade": "#54A24B",
}


def set_paper_style():
    plt.rcParams.update({
        "font.family": "serif",
        "font.serif": ["Times New Roman", "Times", "DejaVu Serif"],
        "font.size": 8,
        "axes.labelsize": 9,
        "axes.titlesize": 9,
        "xtick.labelsize": 8,
        "ytick.labelsize": 8,
        "legend.fontsize": 8,
        "figure.dpi": 300,
        "savefig.dpi": 300,
    })


def plot_fig4(e1_summary: Path, out_png: Path, out_pdf: Path):
    rows = list(csv.DictReader(open(e1_summary)))
    modes = ["baseline", "observe", "intervene"]
    ns = sorted({int(r["N"]) for r in rows})
    data = {(int(r["N"]), r["mode"]): r for r in rows}

    fig, ax = plt.subplots(figsize=(3.4, 2.4))
    width = 0.22
    x = list(range(len(ns)))
    for i, mode in enumerate(modes):
        means = [float(data[(n, mode)]["mean_step_us_mean"]) for n in ns]
        lows = [float(data[(n, mode)]["mean_step_us_ci_low"]) for n in ns]
        highs = [float(data[(n, mode)]["mean_step_us_ci_high"]) for n in ns]
        yerr = [
            [m - lo for m, lo in zip(means, lows)],
            [hi - m for m, hi in zip(means, highs)],
        ]
        ax.bar([p + (i - 1) * width for p in x], means, width=width,
               color=COLORS[mode], edgecolor="black", linewidth=0.6,
               yerr=yerr, capsize=3, label=mode)

    ax.set_xticks(x)
    ax.set_xticklabels([str(n) for n in ns])
    ax.set_xlabel("Ready-set size N")
    ax.set_ylabel("Mean step time (us)")
    ax.legend(frameon=False)
    ax.grid(axis="y", alpha=0.25)
    fig.tight_layout()
    out_png.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_png)
    fig.savefig(out_pdf)


def plot_fig5(e2_plotdata: Path, out_png: Path, out_pdf: Path):
    rows = list(csv.DictReader(open(e2_plotdata)))
    labels = [r["method"] for r in rows]
    means = [float(r["mean"]) for r in rows]
    lows = [float(r["ci_low"]) for r in rows]
    highs = [float(r["ci_high"]) for r in rows]
    colors = ["#4C78A8", "#F58518", "#54A24B"]
    xs = list(range(len(rows)))

    fig, ax = plt.subplots(figsize=(3.4, 2.5))
    yerr = [[m - lo for m, lo in zip(means, lows)], [hi - m for m, hi in zip(means, highs)]]
    ax.bar(xs, means, color=colors, edgecolor="black", linewidth=0.6, yerr=yerr, capsize=3)
    ax.set_xticks(xs)
    ax.set_xticklabels(["LF baseline", "MS+RT\nsingle-worker", "MS+RT\nworker-group"])
    ax.set_ylabel("HC miss rate (%)")
    ax.grid(axis="y", alpha=0.25)
    for x, m in zip(xs, means):
        ax.text(x, m + 1.0, f"{m:.1f}", ha="center", va="bottom", fontsize=7)
    fig.tight_layout()
    out_png.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_png)
    fig.savefig(out_pdf)


def plot_e3(e3_summary: Path, out_png: Path, out_pdf: Path):
    rows = list(csv.DictReader(open(e3_summary)))
    arms = ["baseline", "ms", "degrade"]
    data = {arm: [] for arm in arms}
    for arm in arms:
        arm_rows = [r for r in rows if r["arm"] == arm]
        arm_rows.sort(key=lambda r: float(r["load_factor"]))
        data[arm] = arm_rows

    fig, ax = plt.subplots(figsize=(3.4, 2.5))
    labels = {"baseline": "Baseline", "ms": "MS-only", "degrade": "MS+degrade"}
    for arm in arms:
        xs = [float(r["load_factor"]) for r in data[arm]]
        ys = [float(r["hc_miss_rate_mean"]) for r in data[arm]]
        lo = [float(r["hc_miss_rate_ci_low"]) for r in data[arm]]
        hi = [float(r["hc_miss_rate_ci_high"]) for r in data[arm]]
        ax.plot(xs, ys, marker="o", linewidth=1.4, markersize=3.5, color=COLORS[arm], label=labels[arm])
        ax.fill_between(xs, lo, hi, color=COLORS[arm], alpha=0.18)
    ax.set_xlabel("LC load factor")
    ax.set_ylabel("HC miss rate (%)")
    ax.grid(alpha=0.25)
    ax.legend(frameon=False)
    fig.tight_layout()
    out_png.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_png)
    fig.savefig(out_pdf)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--e1-summary", default="ms-eval/results/e1_three_conditions_summary.csv")
    ap.add_argument("--e2-plotdata", default="ms-eval/results/e2_rt_comparison_plotdata_v2.csv")
    ap.add_argument("--e3-summary", default="ms-eval/results/e3_followup_summary_n30.csv")
    ap.add_argument("--fig4-png", default="ms-eval/figures/fig4_e1_overhead.png")
    ap.add_argument("--fig4-pdf", default="ms-eval/figures/fig4_e1_overhead.pdf")
    ap.add_argument("--fig5-png", default="ms-eval/figures/fig5_e2_missrate.png")
    ap.add_argument("--fig5-pdf", default="ms-eval/figures/fig5_e2_missrate.pdf")
    ap.add_argument("--e3-png", default="ms-eval/figures/fig_e3_loadsweep.png")
    ap.add_argument("--e3-pdf", default="ms-eval/figures/fig_e3_loadsweep.pdf")
    args = ap.parse_args()

    set_paper_style()
    plot_fig4(Path(args.e1_summary), Path(args.fig4_png), Path(args.fig4_pdf))
    plot_fig5(Path(args.e2_plotdata), Path(args.fig5_png), Path(args.fig5_pdf))
    plot_e3(Path(args.e3_summary), Path(args.e3_png), Path(args.e3_pdf))


if __name__ == "__main__":
    main()
