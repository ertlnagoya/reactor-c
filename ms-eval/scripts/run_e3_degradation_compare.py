#!/usr/bin/env python3
import argparse
import csv
import math
import re
import subprocess
from collections import defaultdict
from pathlib import Path
from statistics import mean, stdev

ROOT = Path("/Users/yutaka/program/reactor-c")
PARSE_E3 = ROOT / "ms-eval/scripts/parse_e3.py"


def run_cmd(cmd: str) -> str:
    p = subprocess.run(cmd, shell=True, text=True, capture_output=True)
    if p.returncode != 0:
      raise RuntimeError(f"command failed: {cmd}\nstdout:\n{p.stdout}\nstderr:\n{p.stderr}")
    return p.stdout + p.stderr


def latest_ts(output: str) -> str:
    matches = re.findall(r"\b\d{8}_\d{6}\b", output)
    if not matches:
        raise RuntimeError("timestamp not found in run output")
    return matches[-1]


def read_mode_rows(path: Path) -> dict[tuple[str, float], dict]:
    rows = {}
    with path.open() as f:
        reader = csv.DictReader(f)
        for row in reader:
            rows[(row["mode"], float(row["load_factor"]))] = row
    return rows


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
    ap.add_argument("--loads", default="1.0,1.4,1.8")
    ap.add_argument("--repeats", type=int, default=8)
    ap.add_argument("--steps", type=int, default=300)
    ap.add_argument("--workers", type=int, default=2)
    ap.add_argument("--hc", type=int, default=2)
    ap.add_argument("--lc", type=int, default=6)
    ap.add_argument("--hc-work-us", type=int, default=180)
    ap.add_argument("--lc-work-us", type=int, default=220)
    ap.add_argument("--deadline-us", type=int, default=900)
    ap.add_argument("--period-us", type=int, default=1000)
    ap.add_argument("--degrade-lag-ns", type=int, default=150000)
    ap.add_argument("--degrade-ready-q-len", type=int, default=2)
    ap.add_argument("--drop-initial-tags", type=int, default=20)
    ap.add_argument("--out-prefix", default="e3_degradation_compare")
    args = ap.parse_args()

    loads = [float(x.strip()) for x in args.loads.split(",") if x.strip()]
    load_s = ",".join(str(x) for x in loads)
    long_rows = []
    skip_compile = 0

    for rep in range(1, args.repeats + 1):
        cmd = (
            f"./run_e3.sh --workers {args.workers} --hc {args.hc} --lc {args.lc} "
            f"--steps {args.steps} --hc-work-us {args.hc_work_us} --lc-work-us {args.lc_work_us} "
            f"--hc-deadline-us {args.deadline_us} --lc-deadline-us {args.deadline_us} "
            f"--period-us {args.period_us} --load-factors {load_s} "
            f"--degrade-lag-ns {args.degrade_lag_ns} --degrade-ready-q-len {args.degrade_ready_q_len} "
            f"--skip-compile {skip_compile}"
        )
        output = run_cmd(cmd)
        skip_compile = 1
        ts = latest_ts(output)

        tmp_csv = ROOT / f"ms-eval/results/_tmp_e3_{ts}.csv"
        run_cmd(
            f"python3 {PARSE_E3} --logs {ROOT}/ms-eval/logs/e3/{ts} "
            f"--out {tmp_csv} --drop-initial-tags {args.drop_initial_tags}"
        )
        rows = read_mode_rows(tmp_csv)
        tmp_csv.unlink(missing_ok=True)

        for load in loads:
            baseline = rows[("baseline", load)]
            ms = rows[("ms", load)]
            degrade = rows[("degrade", load)]
            long_rows.append([
                rep,
                f"{load:.2f}",
                ts,
                baseline["hc_miss_rate"],
                ms["hc_miss_rate"],
                degrade["hc_miss_rate"],
                baseline["hc_latency_mean_us"],
                ms["hc_latency_mean_us"],
                degrade["hc_latency_mean_us"],
                baseline["hc_latency_p95_us"],
                ms["hc_latency_p95_us"],
                degrade["hc_latency_p95_us"],
                baseline["lc_completion_ratio"],
                ms["lc_completion_ratio"],
                degrade["lc_completion_ratio"],
                degrade["lc_degraded_count"],
                degrade["fallback_count"],
                degrade["fallback_missing_metadata"],
                degrade["fallback_no_candidate"],
                degrade["violations"],
            ])

    results_dir = ROOT / "ms-eval/results"
    results_dir.mkdir(parents=True, exist_ok=True)
    long_path = results_dir / f"{args.out_prefix}_long.csv"
    with long_path.open("w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow([
            "rep", "load_factor", "ts",
            "baseline_hc_miss_rate", "ms_hc_miss_rate", "degrade_hc_miss_rate",
            "baseline_hc_latency_mean_us", "ms_hc_latency_mean_us", "degrade_hc_latency_mean_us",
            "baseline_hc_latency_p95_us", "ms_hc_latency_p95_us", "degrade_hc_latency_p95_us",
            "baseline_lc_completion_ratio", "ms_lc_completion_ratio", "degrade_lc_completion_ratio",
            "degrade_actions", "degrade_fallback_count",
            "degrade_fallback_missing_metadata", "degrade_fallback_no_candidate",
            "degrade_violations",
        ])
        writer.writerows(long_rows)

    by_load = defaultdict(lambda: defaultdict(list))
    for row in long_rows:
        load = float(row[1])
        by_load[load]["baseline_hc"].append(float(row[3]))
        by_load[load]["ms_hc"].append(float(row[4]))
        by_load[load]["degrade_hc"].append(float(row[5]))
        by_load[load]["baseline_hc_lat_mean"].append(float(row[6]))
        by_load[load]["ms_hc_lat_mean"].append(float(row[7]))
        by_load[load]["degrade_hc_lat_mean"].append(float(row[8]))
        by_load[load]["baseline_hc_lat_p95"].append(float(row[9]))
        by_load[load]["ms_hc_lat_p95"].append(float(row[10]))
        by_load[load]["degrade_hc_lat_p95"].append(float(row[11]))
        by_load[load]["baseline_lc"].append(float(row[12]))
        by_load[load]["ms_lc"].append(float(row[13]))
        by_load[load]["degrade_lc"].append(float(row[14]))
        by_load[load]["degrade_actions"].append(float(row[15]))
        by_load[load]["fallbacks"].append(float(row[16]))
        by_load[load]["fallback_missing_metadata"].append(float(row[17]))
        by_load[load]["fallback_no_candidate"].append(float(row[18]))
        by_load[load]["violations"].append(float(row[19]))

    summary_path = results_dir / f"{args.out_prefix}_summary.csv"
    with summary_path.open("w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow([
            "load_factor",
            "baseline_hc_miss_mean", "baseline_hc_miss_ci_low", "baseline_hc_miss_ci_high",
            "ms_hc_miss_mean", "ms_hc_miss_ci_low", "ms_hc_miss_ci_high",
            "degrade_hc_miss_mean", "degrade_hc_miss_ci_low", "degrade_hc_miss_ci_high",
            "baseline_hc_latency_mean_us", "ms_hc_latency_mean_us", "degrade_hc_latency_mean_us",
            "baseline_hc_latency_p95_us", "ms_hc_latency_p95_us", "degrade_hc_latency_p95_us",
            "baseline_lc_completion_mean", "baseline_lc_completion_ci_low", "baseline_lc_completion_ci_high",
            "ms_lc_completion_mean", "ms_lc_completion_ci_low", "ms_lc_completion_ci_high",
            "degrade_lc_completion_mean", "degrade_lc_completion_ci_low", "degrade_lc_completion_ci_high",
            "degrade_actions_mean", "degrade_actions_ci_low", "degrade_actions_ci_high",
            "fallbacks_mean", "fallbacks_ci_low", "fallbacks_ci_high",
            "fallback_missing_metadata_mean", "fallback_no_candidate_mean", "violations_mean",
            "degrade_minus_ms_hc_miss_mean", "baseline_minus_degrade_hc_miss_mean",
        ])
        for load in sorted(by_load):
            baseline_hc_ci = ci95(by_load[load]["baseline_hc"])
            ms_hc_ci = ci95(by_load[load]["ms_hc"])
            degrade_hc_ci = ci95(by_load[load]["degrade_hc"])
            baseline_lc_ci = ci95(by_load[load]["baseline_lc"])
            ms_lc_ci = ci95(by_load[load]["ms_lc"])
            degrade_lc_ci = ci95(by_load[load]["degrade_lc"])
            actions_ci = ci95(by_load[load]["degrade_actions"])
            fallbacks_ci = ci95(by_load[load]["fallbacks"])
            writer.writerow([
                f"{load:.2f}",
                f"{mean(by_load[load]['baseline_hc']):.3f}", f"{baseline_hc_ci[0]:.3f}", f"{baseline_hc_ci[1]:.3f}",
                f"{mean(by_load[load]['ms_hc']):.3f}", f"{ms_hc_ci[0]:.3f}", f"{ms_hc_ci[1]:.3f}",
                f"{mean(by_load[load]['degrade_hc']):.3f}", f"{degrade_hc_ci[0]:.3f}", f"{degrade_hc_ci[1]:.3f}",
                f"{mean(by_load[load]['baseline_hc_lat_mean']):.3f}",
                f"{mean(by_load[load]['ms_hc_lat_mean']):.3f}",
                f"{mean(by_load[load]['degrade_hc_lat_mean']):.3f}",
                f"{mean(by_load[load]['baseline_hc_lat_p95']):.3f}",
                f"{mean(by_load[load]['ms_hc_lat_p95']):.3f}",
                f"{mean(by_load[load]['degrade_hc_lat_p95']):.3f}",
                f"{mean(by_load[load]['baseline_lc']):.3f}", f"{baseline_lc_ci[0]:.3f}", f"{baseline_lc_ci[1]:.3f}",
                f"{mean(by_load[load]['ms_lc']):.3f}", f"{ms_lc_ci[0]:.3f}", f"{ms_lc_ci[1]:.3f}",
                f"{mean(by_load[load]['degrade_lc']):.3f}", f"{degrade_lc_ci[0]:.3f}", f"{degrade_lc_ci[1]:.3f}",
                f"{mean(by_load[load]['degrade_actions']):.3f}", f"{actions_ci[0]:.3f}", f"{actions_ci[1]:.3f}",
                f"{mean(by_load[load]['fallbacks']):.3f}", f"{fallbacks_ci[0]:.3f}", f"{fallbacks_ci[1]:.3f}",
                f"{mean(by_load[load]['fallback_missing_metadata']):.3f}",
                f"{mean(by_load[load]['fallback_no_candidate']):.3f}",
                f"{mean(by_load[load]['violations']):.3f}",
                f"{mean(by_load[load]['degrade_hc']) - mean(by_load[load]['ms_hc']):.3f}",
                f"{mean(by_load[load]['baseline_hc']) - mean(by_load[load]['degrade_hc']):.3f}",
            ])

    print(long_path)
    print(summary_path)


if __name__ == "__main__":
    main()
