#!/usr/bin/env python3
import argparse
import csv
from collections import defaultdict
from pathlib import Path
from statistics import mean


def read_rows(path: Path) -> list[dict[str, str]]:
    with path.open() as f:
        return list(csv.DictReader(f))


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--thread", default="ms-eval/results/e2_thread_utilization.csv")
    ap.add_argument("--latency", default="ms-eval/results/e2_reaction_latency_distribution.csv")
    ap.add_argument("--single", default="ms-eval/results/e2_single_worker_thread_comparison.csv")
    ap.add_argument("--out", default="ms-eval/results/e2_thread_analysis_summary.csv")
    args = ap.parse_args()

    out_rows = []

    thread_rows = read_rows(Path(args.thread))
    by_worker = defaultdict(list)
    for row in thread_rows:
      if float(row["samples"]) <= 0:
        continue
      by_worker[(row["condition"], row["worker_id"], row["rt_applied"])].append(row)

    for (condition, worker_id, rt_applied), rows in sorted(by_worker.items()):
        out_rows.extend([
            ["thread_utilization", condition, f"worker_{worker_id}", "rt_applied", rt_applied, len(rows)],
            ["thread_utilization", condition, f"worker_{worker_id}", "cpu_pct_mean", f"{mean(float(r['cpu_pct_mean']) for r in rows):.3f}", len(rows)],
            ["thread_utilization", condition, f"worker_{worker_id}", "voluntary_ctx_switch_est", f"{mean(float(r['voluntary_ctx_switch_est']) for r in rows):.3f}", len(rows)],
            ["thread_utilization", condition, f"worker_{worker_id}", "involuntary_ctx_switch_est", f"{mean(float(r['involuntary_ctx_switch_est']) for r in rows):.3f}", len(rows)],
        ])

    latency_rows = read_rows(Path(args.latency))
    by_condition = defaultdict(list)
    for row in latency_rows:
        by_condition[row["condition"]].append(row)

    for condition, rows in sorted(by_condition.items()):
        for metric in [
            "latency_p50_us",
            "latency_p95_us",
            "latency_p99_us",
            "latency_mean_us",
            "hc_latency_mean_us",
            "lc_latency_mean_us",
        ]:
            out_rows.append([
                "latency_distribution",
                condition,
                "all",
                metric,
                f"{mean(float(r[metric]) for r in rows):.3f}",
                len(rows),
            ])

    single_rows = read_rows(Path(args.single))
    for metric in [
        "rt_designated_hc_latency_mean_us",
        "non_rt_designated_lc_latency_mean_us",
        "rt_thread_cpu_pct_mean",
        "non_rt_thread_cpu_pct_mean",
    ]:
        out_rows.append([
            "single_worker_comparison",
            "rt_single_worker",
            "designated",
            metric,
            f"{mean(float(r[metric]) for r in single_rows):.3f}",
            len(single_rows),
        ])

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with out_path.open("w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["category", "condition", "subgroup", "metric", "mean_value", "n"])
        writer.writerows(out_rows)


if __name__ == "__main__":
    main()
