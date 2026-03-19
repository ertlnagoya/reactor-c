#!/usr/bin/env python3
import argparse
import csv
import math
import os
import random
import re
import subprocess
from collections import defaultdict
from pathlib import Path
from statistics import mean, stdev
from typing import Optional

ROOT = Path("/Users/yutaka/program/reactor-c")
EVAL_DIR = ROOT / "ms-eval"
PARSE_E1 = ROOT / "ms-eval/scripts/parse_e1.py"


def run_cmd(cmd: str, env: Optional[dict[str, str]] = None) -> str:
    p = subprocess.run(cmd, shell=True, text=True, capture_output=True, env=env)
    if p.returncode != 0:
        raise RuntimeError(f"command failed: {cmd}\nstdout:\n{p.stdout}\nstderr:\n{p.stderr}")
    return p.stdout + p.stderr


def latest_ts(output: str) -> str:
    matches = re.findall(r"\b\d{8}_\d{6}\b", output)
    if not matches:
        raise RuntimeError("timestamp not found in run output")
    return matches[-1]


def ci95(values: list[float]) -> tuple[float, float]:
    if not values:
        return (math.nan, math.nan)
    if len(values) == 1:
        return (values[0], values[0])
    m = mean(values)
    half = 1.96 * stdev(values) / math.sqrt(len(values))
    return (m - half, m + half)


def compile_for_n(n: int, iterations: int, workload_us: int, deadline_us: int, period_us: int, seed: int) -> str:
    lf_file = EVAL_DIR / "lf-gen" / f"e1_microbench_N{n}.lf"
    gen_cmd = (
        f"{EVAL_DIR}/scripts/gen_microbench.py --out '{lf_file}' --reactions {n} "
        f"--workload-us {workload_us} --deadline-us {deadline_us} "
        f"--period-us {period_us} --steps {iterations} --seed {seed} --experiment e1"
    )
    run_cmd(gen_cmd)
    compile_cmd = (
        f"bash -lc 'export PATH=\"$HOME/Library/Python/3.9/bin:$PATH\"; "
        f"source \"{EVAL_DIR}/scripts/run_common.sh\"; "
        f"compile_lf \"{lf_file}\"'"
    )
    return run_cmd(compile_cmd)


def run_mode_once(name: str, mode: str, log_dir: Path) -> None:
    env = os.environ.copy()
    env["MS_MODE"] = mode
    if mode == "baseline":
        env["LF_MS_DISABLE"] = "1"
        env["LF_MS_OBSERVE_ONLY"] = "0"
    elif mode == "observe":
        env["LF_MS_DISABLE"] = "0"
        env["LF_MS_OBSERVE_ONLY"] = "1"
    else:
        env["LF_MS_DISABLE"] = "0"
        env["LF_MS_OBSERVE_ONLY"] = "0"

    exe = ROOT / f"src-gen/ms-eval/lf-gen/{name}/build/{name}"
    if not exe.exists():
        raise RuntimeError(f"executable not found: {exe}")

    app_log = log_dir / "app.jsonl"
    ms_log = log_dir / "ms.log"
    log_dir.mkdir(parents=True, exist_ok=True)
    run_cmd(
        f"LF_APP_LOG='{app_log}' LF_MS_LOG='{ms_log}' LF_MS_LOG_LEVEL=INFO '{exe}'",
        env=env,
    )


def parse_dir(logs: Path, out_csv: Path) -> list[dict[str, str]]:
    run_cmd(f"python3 {PARSE_E1} --logs {logs} --out {out_csv}")
    with out_csv.open() as f:
        return list(csv.DictReader(f))


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--Ns", default="16,32,64,128")
    ap.add_argument("--repeats", type=int, default=30)
    ap.add_argument("--warmup-runs", type=int, default=3)
    ap.add_argument("--iterations", type=int, default=1000)
    ap.add_argument("--workload-us", type=int, default=50)
    ap.add_argument("--deadline-us", type=int, default=200)
    ap.add_argument("--period-us", type=int, default=1000)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--rand-seed", type=int, default=12345)
    ap.add_argument("--out-prefix", default="e1_overhead_n30")
    args = ap.parse_args()

    ns = [int(x.strip()) for x in args.Ns.split(",") if x.strip()]
    modes = ["baseline", "observe", "intervene"]
    timestamp = run_cmd("date +%Y%m%d_%H%M%S").strip()
    log_root = EVAL_DIR / "logs" / "e1_n30" / timestamp
    rng = random.Random(args.rand_seed)

    for n in ns:
        compile_for_n(n, args.iterations, args.workload_us, args.deadline_us, args.period_us, args.seed)
        name = f"e1_microbench_N{n}"
        schedule = []
        for rep in range(0, args.warmup_runs + args.repeats):
            order = modes[:]
            rng.shuffle(order)
            phase = "warmup" if rep < args.warmup_runs else "measured"
            rep_id = rep + 1 if phase == "warmup" else rep - args.warmup_runs + 1
            for mode in order:
                schedule.append((phase, rep_id, mode))

        for phase, rep_id, mode in schedule:
            log_dir = log_root / f"N{n}" / phase / f"rep{rep_id:02d}" / mode
            run_mode_once(name, mode, log_dir)

    long_rows = []
    stats = defaultdict(lambda: defaultdict(list))
    tmp_csv = ROOT / f"ms-eval/results/_tmp_e1_{timestamp}.csv"
    measured_rows = []
    for app_jsonl in log_root.glob("N*/measured/rep*/**/app.jsonl"):
        rel = app_jsonl.relative_to(log_root)
        n = int(rel.parts[0][1:])
        rep = int(rel.parts[2][3:])
        mode = rel.parts[3]
        parsed = parse_dir(app_jsonl.parent, tmp_csv)
        tmp_csv.unlink(missing_ok=True)
        if not parsed:
            continue
        row = parsed[0]
        measured_rows.append({
            "N": n,
            "mode": mode,
            "rep": rep,
            "iterations": int(row["iterations"]),
            "mean_step_us": float(row["mean_step_us"]),
            "p50_step_us": float(row["p50_step_us"]),
            "p95_step_us": float(row["p95_step_us"]),
        })

    baseline_by_rep = {}
    for row in measured_rows:
        if row["mode"] == "baseline":
            baseline_by_rep[(row["N"], row["rep"])] = row["mean_step_us"]

    for row in sorted(measured_rows, key=lambda r: (r["N"], r["rep"], r["mode"])):
        base = baseline_by_rep[(row["N"], row["rep"])]
        overhead_pct = ((row["mean_step_us"] - base) / base * 100.0) if base > 0 else 0.0
        long_rows.append([
            row["N"],
            row["mode"],
            row["rep"],
            row["iterations"],
            f"{row['mean_step_us']:.3f}",
            f"{row['p50_step_us']:.3f}",
            f"{row['p95_step_us']:.3f}",
            f"{overhead_pct:.3f}",
        ])
        stats[(row["N"], row["mode"])]["mean_step_us"].append(row["mean_step_us"])
        stats[(row["N"], row["mode"])]["overhead_pct"].append(overhead_pct)

    results_dir = ROOT / "ms-eval/results"
    results_dir.mkdir(parents=True, exist_ok=True)
    raw_path = results_dir / f"{args.out_prefix}.csv"
    with raw_path.open("w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow([
            "N", "mode", "run_id", "iterations", "mean_step_us", "p50_step_us", "p95_step_us", "overhead_pct"
        ])
        writer.writerows(long_rows)

    summary_path = results_dir / f"{args.out_prefix}_summary.csv"
    with summary_path.open("w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow([
            "N", "mode", "n",
            "mean_step_us_mean", "mean_step_us_std", "mean_step_us_ci_low", "mean_step_us_ci_high",
            "overhead_pct_mean", "overhead_pct_std", "overhead_pct_ci_low", "overhead_pct_ci_high",
        ])
        for n, mode in sorted(stats.keys(), key=lambda x: (x[0], x[1])):
            step_vals = stats[(n, mode)]["mean_step_us"]
            overhead_vals = stats[(n, mode)]["overhead_pct"]
            step_ci = ci95(step_vals)
            overhead_ci = ci95(overhead_vals)
            writer.writerow([
                n,
                mode,
                len(step_vals),
                f"{mean(step_vals):.3f}",
                f"{(stdev(step_vals) if len(step_vals) > 1 else 0.0):.3f}",
                f"{step_ci[0]:.3f}",
                f"{step_ci[1]:.3f}",
                f"{mean(overhead_vals):.3f}",
                f"{(stdev(overhead_vals) if len(overhead_vals) > 1 else 0.0):.3f}",
                f"{overhead_ci[0]:.3f}",
                f"{overhead_ci[1]:.3f}",
            ])

    print(raw_path)
    print(summary_path)
    print(log_root)


if __name__ == "__main__":
    main()
