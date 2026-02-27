#!/usr/bin/env python3
import argparse
import csv
import math
from pathlib import Path

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt


def read_first(path: Path):
    with path.open() as f:
        return next(csv.DictReader(f))


def val(row, key):
    x = row.get(key, '')
    try:
        return float(x)
    except Exception:
        return math.nan


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--baseline', default='ms-eval/results/e1_rtgroup_main_r30_summary.csv')
    ap.add_argument('--comparison', default='ms-eval/results/e1_rtaxis_fix_rt_guard_r30_summary.csv')
    ap.add_argument('--proposed', default='ms-eval/results/e1_rtgroup_main_r30_summary.csv')
    ap.add_argument('--out-png', default='ms-eval/figures/fig_e2_rt_comparison.png')
    ap.add_argument('--out-pdf', default='ms-eval/figures/fig_e2_rt_comparison.pdf')
    ap.add_argument('--out-csv', default='ms-eval/results/e2_rt_comparison_plotdata.csv')
    args = ap.parse_args()

    # Baseline uses policy_avg from baseline summary (no OS control path)
    b = read_first(Path(args.baseline))
    c = read_first(Path(args.comparison))
    p = read_first(Path(args.proposed))

    labels = ['baseline (no OS)', 'comparison (RT single-worker)', 'proposed (RT group)']
    means = [
        val(b, 'policy_avg_mean'),
        val(c, 'policy_os_mean'),
        val(p, 'policy_os_mean'),
    ]
    lows = [
        val(b, 'policy_avg_ci_low'),
        val(c, 'policy_os_ci_low'),
        val(p, 'policy_os_ci_low'),
    ]
    highs = [
        val(b, 'policy_avg_ci_high'),
        val(c, 'policy_os_ci_high'),
        val(p, 'policy_os_ci_high'),
    ]

    yerr = [[max(0.0, m - lo) for m, lo in zip(means, lows)], [max(0.0, hi - m) for m, hi in zip(means, highs)]]

    colors = ['#4C78A8', '#F58518', '#54A24B']

    fig, ax = plt.subplots(figsize=(8, 4.4))
    xs = list(range(len(labels)))
    ax.bar(xs, means, yerr=yerr, capsize=6, color=colors, alpha=0.9, edgecolor='black', linewidth=0.7)
    ax.set_xticks(xs)
    ax.set_xticklabels(labels, rotation=8, ha='right')
    ax.set_ylabel('HC miss rate (%)')
    ax.set_title('E2 Replacement: Baseline vs RT Single-Worker vs RT Group')
    ax.grid(axis='y', alpha=0.3)
    ax.set_ylim(bottom=0)

    # Annotate means
    for x, m in zip(xs, means):
        ax.text(x, m + 1.2, f'{m:.2f}', ha='center', va='bottom', fontsize=9)

    fig.tight_layout()
    Path(args.out_png).parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(args.out_png, dpi=200)
    fig.savefig(args.out_pdf)

    Path(args.out_csv).parent.mkdir(parents=True, exist_ok=True)
    with Path(args.out_csv).open('w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['method', 'mean', 'ci_low', 'ci_high'])
        for lab, m, lo, hi in zip(labels, means, lows, highs):
            w.writerow([lab, m, lo, hi])


if __name__ == '__main__':
    main()
