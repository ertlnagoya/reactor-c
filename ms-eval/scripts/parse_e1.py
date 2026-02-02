#!/usr/bin/env python3
import argparse
from pathlib import Path
from collections import defaultdict

from common_parse import read_jsonl, percentile, write_csv


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--logs", default="ms-eval/logs/e1")
    ap.add_argument("--out", default="ms-eval/results/e1_overhead.csv")
    args = ap.parse_args()

    log_root = Path(args.logs)
    runs = []

    for run_dir in log_root.glob("**/app.jsonl"):
        events = list(read_jsonl(run_dir))
        if not events:
            continue
        run_meta = next((e for e in events if e.get("type") == "run_start"), None)
        if not run_meta:
            continue
        starts = defaultdict(list)
        ends = defaultdict(list)
        for e in events:
            if e.get("type") == "reaction_start":
                starts[e["logical_time_ns"]].append(e["ts_mono_ns"])
            elif e.get("type") == "reaction_end":
                ends[e["logical_time_ns"]].append(e["ts_mono_ns"])
        durations = []
        for logical_time in starts:
            if logical_time not in ends:
                continue
            durations.append(max(ends[logical_time]) - min(starts[logical_time]))
        durations.sort()
        if not durations:
            continue
        runs.append({
            "mode": run_meta.get("mode", ""),
            "N": run_meta.get("reactions", 0),
            "workload_us": run_meta.get("workload_us", 0),
            "iterations": run_meta.get("steps", 0),
            "mean_step_us": sum(durations) / len(durations) / 1000.0,
            "p50_step_us": percentile(durations, 50) / 1000.0,
            "p95_step_us": percentile(durations, 95) / 1000.0,
        })

    baseline = {}
    for r in runs:
        if r["mode"] != "baseline":
            continue
        key = (r["N"], r["workload_us"])
        baseline[key] = r["mean_step_us"]

    rows = []
    for r in runs:
        key = (r["N"], r["workload_us"])
        base = baseline.get(key)
        overhead = 0.0
        if base and base > 0:
            overhead = (r["mean_step_us"] - base) / base * 100.0
        rows.append([
            r["mode"],
            r["N"],
            r["workload_us"],
            r["iterations"],
            f"{r['mean_step_us']:.3f}",
            f"{r['p50_step_us']:.3f}",
            f"{r['p95_step_us']:.3f}",
            f"{overhead:.2f}",
        ])

    header = [
        "mode",
        "N",
        "workload_us",
        "iterations",
        "mean_step_us",
        "p50_step_us",
        "p95_step_us",
        "overhead_pct",
    ]
    write_csv(Path(args.out), header, rows)


if __name__ == "__main__":
    main()
