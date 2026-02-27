#!/usr/bin/env python3
import argparse
import csv
import math
from pathlib import Path
from statistics import mean, stdev

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt


def ci95(vals):
    if not vals:
        return (math.nan, math.nan)
    if len(vals) == 1:
        return (vals[0], vals[0])
    m = mean(vals)
    s = stdev(vals)
    h = 1.96 * s / math.sqrt(len(vals))
    return (m - h, m + h)


def read_long(path):
    with open(path) as f:
        rows = list(csv.DictReader(f))
    b = []
    osv = []
    for r in rows:
        # True baseline: MS disabled path in each run.
        b1 = float(r['baseline_policy1'])
        b2 = float(r['baseline_policy2'])
        b.append(0.5 * (b1 + b2))
        osv.append(float(r['policy_os']))
    return b, osv


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--comparison-long', default='ms-eval/results/e1_rtaxis_fix_rt_guard_r30_long.csv')
    ap.add_argument('--proposed-long', default='ms-eval/results/e1_rtgroup_main_r30_long.csv')
    ap.add_argument('--out-png', default='ms-eval/figures/fig_e2_rt_comparison_v2.png')
    ap.add_argument('--out-pdf', default='ms-eval/figures/fig_e2_rt_comparison_v2.pdf')
    ap.add_argument('--out-csv', default='ms-eval/results/e2_rt_comparison_plotdata_v2.csv')
    args = ap.parse_args()

    b_cmp, os_cmp = read_long(args.comparison_long)
    b_prop, os_prop = read_long(args.proposed_long)

    # baseline: pooled LF baseline from both experiment sets (MS disabled).
    baseline_vals = b_cmp + b_prop

    labels = [
        'LF baseline (MS disabled)',
        'MS + RT (single-worker)',
        'MS + RT (worker-group, proposed)',
    ]
    vals = [baseline_vals, os_cmp, os_prop]
    means = [mean(v) for v in vals]
    cis = [ci95(v) for v in vals]
    lows = [c[0] for c in cis]
    highs = [c[1] for c in cis]

    yerr = [
        [max(0.0, m - lo) for m, lo in zip(means, lows)],
        [max(0.0, hi - m) for m, hi in zip(means, highs)],
    ]

    fig, ax = plt.subplots(figsize=(8.4, 4.6))
    colors = ['#4C78A8', '#F58518', '#54A24B']
    xs = list(range(3))
    ax.bar(xs, means, yerr=yerr, capsize=6, color=colors, edgecolor='black', linewidth=0.7)
    ax.set_xticks(xs)
    ax.set_xticklabels(labels, rotation=7, ha='right')
    ax.set_ylabel('HC miss rate (%)')
    ax.set_title('E2: HC Miss Rate (Lower is Better)')
    ax.grid(axis='y', alpha=0.3)
    ax.set_ylim(bottom=0)

    for x, m in zip(xs, means):
        ax.text(x, m + 1.2, f'{m:.2f}', ha='center', va='bottom', fontsize=9)

    fig.tight_layout()
    Path(args.out_png).parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(args.out_png, dpi=220)
    fig.savefig(args.out_pdf)

    Path(args.out_csv).parent.mkdir(parents=True, exist_ok=True)
    with open(args.out_csv, 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['method', 'mean', 'ci_low', 'ci_high', 'n'])
        for label, m, lo, hi, v in zip(labels, means, lows, highs, vals):
            w.writerow([label, m, lo, hi, len(v)])


if __name__ == '__main__':
    main()
