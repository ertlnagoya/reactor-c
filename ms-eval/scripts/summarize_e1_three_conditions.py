#!/usr/bin/env python3
import argparse
import csv
import math
from collections import defaultdict
from pathlib import Path
from statistics import mean, stdev


def ci95(values: list[float]) -> tuple[float, float]:
    if not values:
        return (math.nan, math.nan)
    if len(values) == 1:
        return (values[0], values[0])
    m = mean(values)
    half = 1.96 * stdev(values) / math.sqrt(len(values))
    return (m - half, m + half)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--in-raw", default="ms-eval/results/e1_overhead_n30.csv")
    ap.add_argument("--out-summary", default="ms-eval/results/e1_three_conditions_summary.csv")
    ap.add_argument("--out-diff", default="ms-eval/results/e1_observe_vs_intervene.csv")
    args = ap.parse_args()

    rows = list(csv.DictReader(open(args.in_raw)))
    by_key = defaultdict(lambda: defaultdict(list))
    by_n_run = defaultdict(dict)

    for row in rows:
        n = int(row["N"])
        mode = row["mode"]
        run_id = int(row["run_id"])
        mean_step = float(row["mean_step_us"])
        overhead = float(row["overhead_pct"])
        by_key[(n, mode)]["mean_step_us"].append(mean_step)
        by_key[(n, mode)]["overhead_pct"].append(overhead)
        by_n_run[(n, run_id)][mode] = row

    with Path(args.out_summary).open("w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow([
            "N", "mode", "n",
            "mean_step_us_mean", "mean_step_us_std", "mean_step_us_ci_low", "mean_step_us_ci_high",
            "overhead_pct_mean", "overhead_pct_std", "overhead_pct_ci_low", "overhead_pct_ci_high",
        ])
        for n, mode in sorted(by_key.keys(), key=lambda x: (x[0], x[1])):
            step_vals = by_key[(n, mode)]["mean_step_us"]
            over_vals = by_key[(n, mode)]["overhead_pct"]
            step_ci = ci95(step_vals)
            over_ci = ci95(over_vals)
            writer.writerow([
                n, mode, len(step_vals),
                f"{mean(step_vals):.3f}",
                f"{(stdev(step_vals) if len(step_vals) > 1 else 0.0):.3f}",
                f"{step_ci[0]:.3f}", f"{step_ci[1]:.3f}",
                f"{mean(over_vals):.3f}",
                f"{(stdev(over_vals) if len(over_vals) > 1 else 0.0):.3f}",
                f"{over_ci[0]:.3f}", f"{over_ci[1]:.3f}",
            ])

    with Path(args.out_diff).open("w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow([
            "N", "run_id",
            "observe_mean_step_us", "intervene_mean_step_us",
            "observe_minus_intervene_mean_step_us",
            "observe_overhead_pct", "intervene_overhead_pct",
            "observe_minus_intervene_overhead_pct",
        ])
        for (n, run_id), group in sorted(by_n_run.items()):
            obs = group.get("observe")
            itv = group.get("intervene")
            if not obs or not itv:
                continue
            writer.writerow([
                n, run_id,
                obs["mean_step_us"], itv["mean_step_us"],
                f"{float(obs['mean_step_us']) - float(itv['mean_step_us']):.3f}",
                obs["overhead_pct"], itv["overhead_pct"],
                f"{float(obs['overhead_pct']) - float(itv['overhead_pct']):.3f}",
            ])


if __name__ == "__main__":
    main()
