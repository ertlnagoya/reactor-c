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


def rank_abs_diffs(values: list[float]) -> list[float]:
    indexed = sorted(enumerate(values), key=lambda x: x[1])
    ranks = [0.0] * len(values)
    i = 0
    while i < len(indexed):
        j = i + 1
        while j < len(indexed) and indexed[j][1] == indexed[i][1]:
            j += 1
        avg_rank = (i + 1 + j) / 2.0
        for k in range(i, j):
            ranks[indexed[k][0]] = avg_rank
        i = j
    return ranks


def wilcoxon_signed_rank(xs: list[float], ys: list[float]) -> tuple[int, float, float, float]:
    diffs = [x - y for x, y in zip(xs, ys)]
    nonzero = [d for d in diffs if abs(d) > 1e-12]
    n = len(nonzero)
    if n == 0:
        return (0, 0.0, 1.0, 0.0)

    abs_diffs = [abs(d) for d in nonzero]
    ranks = rank_abs_diffs(abs_diffs)
    w_plus = sum(rank for diff, rank in zip(nonzero, ranks) if diff > 0)
    w_minus = sum(rank for diff, rank in zip(nonzero, ranks) if diff < 0)

    mu = n * (n + 1) / 4.0
    tie_counts = defaultdict(int)
    for value in abs_diffs:
        tie_counts[value] += 1
    tie_term = sum(t * (t + 1) * (2 * t + 1) for t in tie_counts.values() if t > 1)
    sigma_sq = n * (n + 1) * (2 * n + 1) / 24.0 - tie_term / 48.0
    sigma = math.sqrt(max(sigma_sq, 0.0))

    if sigma == 0.0:
        return (n, w_plus, 1.0, 0.0)

    correction = 0.5 if w_plus > mu else -0.5 if w_plus < mu else 0.0
    z = (w_plus - mu - correction) / sigma
    p_value = math.erfc(abs(z) / math.sqrt(2.0))
    effect_r = abs(z) / math.sqrt(n)
    return (n, w_plus, p_value, effect_r)


def read_rows(path: Path) -> list[dict[str, str]]:
    with path.open() as f:
        return list(csv.DictReader(f))


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--in-long", required=True)
    ap.add_argument("--out-raw", required=True)
    ap.add_argument("--out-summary", required=True)
    ap.add_argument("--out-tests", required=True)
    args = ap.parse_args()

    rows = read_rows(Path(args.in_long))
    raw_rows = []
    by_key = defaultdict(lambda: defaultdict(list))
    paired = defaultdict(lambda: defaultdict(list))

    arm_fields = {
        "baseline": (
            "baseline_hc_miss_rate",
            "baseline_lc_completion_ratio",
            None,
            None,
            "baseline_hc_latency_mean_us",
            "baseline_hc_latency_p95_us",
        ),
        "ms": (
            "ms_hc_miss_rate",
            "ms_lc_completion_ratio",
            None,
            None,
            "ms_hc_latency_mean_us",
            "ms_hc_latency_p95_us",
        ),
        "degrade": (
            "degrade_hc_miss_rate",
            "degrade_lc_completion_ratio",
            "degrade_actions",
            "degrade_fallback_count",
            "degrade_hc_latency_mean_us",
            "degrade_hc_latency_p95_us",
        ),
    }

    for row in rows:
        rep = int(row["rep"])
        load = row["load_factor"]
        for arm, fields in arm_fields.items():
            miss_field, lc_field, degrade_field, fallback_field, lat_mean_field, lat_p95_field = fields
            raw = {
                "load_factor": load,
                "arm": arm,
                "run_id": str(rep),
                "hc_miss_rate": row[miss_field],
                "lc_completion_ratio": row[lc_field],
                "degradation_actions": row[degrade_field] if degrade_field else "0",
                "fallback_count": row[fallback_field] if fallback_field else "0",
                "hc_latency_mean_us": row[lat_mean_field],
                "hc_latency_p95_us": row[lat_p95_field],
            }
            raw_rows.append(raw)

            key = (load, arm)
            by_key[key]["hc_miss_rate"].append(float(raw["hc_miss_rate"]))
            by_key[key]["lc_completion_ratio"].append(float(raw["lc_completion_ratio"]))
            by_key[key]["degradation_actions"].append(float(raw["degradation_actions"]))
            by_key[key]["fallback_count"].append(float(raw["fallback_count"]))
            by_key[key]["hc_latency_mean_us"].append(float(raw["hc_latency_mean_us"]))
            by_key[key]["hc_latency_p95_us"].append(float(raw["hc_latency_p95_us"]))

        paired[load]["baseline_hc_miss_rate"].append(float(row["baseline_hc_miss_rate"]))
        paired[load]["degrade_hc_miss_rate"].append(float(row["degrade_hc_miss_rate"]))

    raw_header = [
        "load_factor",
        "arm",
        "run_id",
        "hc_miss_rate",
        "lc_completion_ratio",
        "degradation_actions",
        "fallback_count",
        "hc_latency_mean_us",
        "hc_latency_p95_us",
    ]
    with Path(args.out_raw).open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=raw_header)
        writer.writeheader()
        writer.writerows(raw_rows)

    summary_header = [
        "load_factor",
        "arm",
        "n",
        "hc_miss_rate_mean",
        "hc_miss_rate_std",
        "hc_miss_rate_ci_low",
        "hc_miss_rate_ci_high",
        "lc_completion_ratio_mean",
        "lc_completion_ratio_std",
        "lc_completion_ratio_ci_low",
        "lc_completion_ratio_ci_high",
        "degradation_actions_mean",
        "degradation_actions_std",
        "degradation_actions_ci_low",
        "degradation_actions_ci_high",
        "fallback_count_mean",
        "fallback_count_std",
        "fallback_count_ci_low",
        "fallback_count_ci_high",
        "hc_latency_mean_us_mean",
        "hc_latency_mean_us_std",
        "hc_latency_mean_us_ci_low",
        "hc_latency_mean_us_ci_high",
        "hc_latency_p95_us_mean",
        "hc_latency_p95_us_std",
        "hc_latency_p95_us_ci_low",
        "hc_latency_p95_us_ci_high",
    ]
    with Path(args.out_summary).open("w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(summary_header)
        for load, arm in sorted(by_key.keys(), key=lambda x: (float(x[0]), x[1])):
            metrics = by_key[(load, arm)]
            miss_ci = ci95(metrics["hc_miss_rate"])
            lc_ci = ci95(metrics["lc_completion_ratio"])
            action_ci = ci95(metrics["degradation_actions"])
            fallback_ci = ci95(metrics["fallback_count"])
            lat_mean_ci = ci95(metrics["hc_latency_mean_us"])
            lat_p95_ci = ci95(metrics["hc_latency_p95_us"])

            def safe_std(values: list[float]) -> float:
                return stdev(values) if len(values) > 1 else 0.0

            writer.writerow([
                load,
                arm,
                len(metrics["hc_miss_rate"]),
                f"{mean(metrics['hc_miss_rate']):.3f}",
                f"{safe_std(metrics['hc_miss_rate']):.3f}",
                f"{miss_ci[0]:.3f}",
                f"{miss_ci[1]:.3f}",
                f"{mean(metrics['lc_completion_ratio']):.3f}",
                f"{safe_std(metrics['lc_completion_ratio']):.3f}",
                f"{lc_ci[0]:.3f}",
                f"{lc_ci[1]:.3f}",
                f"{mean(metrics['degradation_actions']):.3f}",
                f"{safe_std(metrics['degradation_actions']):.3f}",
                f"{action_ci[0]:.3f}",
                f"{action_ci[1]:.3f}",
                f"{mean(metrics['fallback_count']):.3f}",
                f"{safe_std(metrics['fallback_count']):.3f}",
                f"{fallback_ci[0]:.3f}",
                f"{fallback_ci[1]:.3f}",
                f"{mean(metrics['hc_latency_mean_us']):.3f}",
                f"{safe_std(metrics['hc_latency_mean_us']):.3f}",
                f"{lat_mean_ci[0]:.3f}",
                f"{lat_mean_ci[1]:.3f}",
                f"{mean(metrics['hc_latency_p95_us']):.3f}",
                f"{safe_std(metrics['hc_latency_p95_us']):.3f}",
                f"{lat_p95_ci[0]:.3f}",
                f"{lat_p95_ci[1]:.3f}",
            ])

    with Path(args.out_tests).open("w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow([
            "load_factor",
            "metric",
            "comparison",
            "n_nonzero",
            "w_plus",
            "p_value_two_sided",
            "effect_size_r",
            "baseline_mean",
            "degrade_mean",
        ])
        for load in sorted(paired.keys(), key=float):
            baseline = paired[load]["baseline_hc_miss_rate"]
            degrade = paired[load]["degrade_hc_miss_rate"]
            n_nonzero, w_plus, p_value, effect_r = wilcoxon_signed_rank(baseline, degrade)
            writer.writerow([
                load,
                "hc_miss_rate",
                "baseline_vs_degrade",
                n_nonzero,
                f"{w_plus:.3f}",
                f"{p_value:.6f}",
                f"{effect_r:.6f}",
                f"{mean(baseline):.3f}",
                f"{mean(degrade):.3f}",
            ])


if __name__ == "__main__":
    main()
