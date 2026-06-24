#!/usr/bin/env python3
import argparse
import csv
import os
import subprocess
from pathlib import Path
from typing import Optional


SCRIPT_DIR = Path(__file__).resolve().parent
REACTOR_C = Path(os.environ.get("TCRS_REACTOR", str(SCRIPT_DIR.parents[1])))
DATA = Path(os.environ.get("TCRS_DATA", str(REACTOR_C / "ms-eval/tcrs-data")))
RUN_COMPARE = SCRIPT_DIR / "run_e3_degradation_compare.py"


def run(cmd: list[str], cwd: Path, env_path: Optional[str] = None, dry_run: bool = False) -> str:
    printable = " ".join(cmd)
    if dry_run:
        print(f"[dry-run] {printable}")
        return ""
    env = None
    if env_path:
        import os
        env = dict(os.environ)
        env["PATH"] = f"{env_path}:{env['PATH']}"
    proc = subprocess.run(cmd, cwd=cwd, env=env, text=True, capture_output=True)
    if proc.returncode != 0:
        raise RuntimeError(
            f"command failed: {printable}\nstdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
        )
    return proc.stdout + proc.stderr


def append_summary(summary_csv: Path, rows: list[dict[str, str]], ready_q: int, lag_ns: int, lc_budget: int):
    with summary_csv.open() as f:
        for row in csv.DictReader(f):
            row = dict(row)
            row["degrade_ready_q_len"] = str(ready_q)
            row["degrade_lag_ns"] = str(lag_ns)
            row["lc_budget"] = str(lc_budget)
            rows.append(row)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--ready-q-lens", default="1,2,4")
    ap.add_argument("--lag-ns", default="100000,150000,250000")
    ap.add_argument("--lc-budgets", default="4")
    ap.add_argument("--workers", type=int, default=2)
    ap.add_argument("--loads", default="0.4,0.8,1.2,1.6")
    ap.add_argument("--repeats", type=int, default=20)
    ap.add_argument("--steps", type=int, default=300)
    ap.add_argument("--hc", type=int, default=2)
    ap.add_argument("--lc", type=int, default=6)
    ap.add_argument("--hc-work-us", type=int, default=180)
    ap.add_argument("--lc-work-us", type=int, default=220)
    ap.add_argument("--deadline-us", type=int, default=900)
    ap.add_argument("--period-us", type=int, default=1000)
    ap.add_argument("--cmake-bin-dir", default="/Users/yutaka/Library/Python/3.9/bin")
    ap.add_argument("--out-prefix", default="tcrs_e6_policy")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    ready_qs = [int(x.strip()) for x in args.ready_q_lens.split(",") if x.strip()]
    lags = [int(x.strip()) for x in args.lag_ns.split(",") if x.strip()]
    budgets = [int(x.strip()) for x in args.lc_budgets.split(",") if x.strip()]
    out_dir = DATA / "e6"
    out_dir.mkdir(parents=True, exist_ok=True)
    combined_rows: list[dict[str, str]] = []

    for budget in budgets:
        for ready_q in ready_qs:
            for lag in lags:
                prefix = f"{args.out_prefix}_b{budget}_q{ready_q}_lag{lag}"
                cmd = [
                    "python3", str(RUN_COMPARE),
                    "--workers", str(args.workers),
                    "--loads", args.loads,
                    "--repeats", str(args.repeats),
                    "--steps", str(args.steps),
                    "--hc", str(args.hc),
                    "--lc", str(args.lc),
                    "--lc-budget", str(budget),
                    "--hc-work-us", str(args.hc_work_us),
                    "--lc-work-us", str(args.lc_work_us),
                    "--deadline-us", str(args.deadline_us),
                    "--period-us", str(args.period_us),
                    "--degrade-lag-ns", str(lag),
                    "--degrade-ready-q-len", str(ready_q),
                    "--out-prefix", prefix,
                ]
                run(cmd, REACTOR_C, env_path=args.cmake_bin_dir, dry_run=args.dry_run)

                if args.dry_run:
                    continue
                long_csv = REACTOR_C / f"ms-eval/results/{prefix}_long.csv"
                summary_csv = REACTOR_C / f"ms-eval/results/{prefix}_summary.csv"
                if not summary_csv.exists():
                    raise FileNotFoundError(summary_csv)
                if long_csv.exists():
                    (out_dir / long_csv.name).write_bytes(long_csv.read_bytes())
                (out_dir / summary_csv.name).write_bytes(summary_csv.read_bytes())
                append_summary(summary_csv, combined_rows, ready_q, lag, budget)

    if not args.dry_run:
        out_csv = out_dir / f"{args.out_prefix}_combined_summary.csv"
        if combined_rows:
            fields = [
                "lc_budget", "degrade_ready_q_len", "degrade_lag_ns",
                *[k for k in combined_rows[0].keys()
                  if k not in {"lc_budget", "degrade_ready_q_len", "degrade_lag_ns"}],
            ]
            with out_csv.open("w", newline="") as f:
                writer = csv.DictWriter(f, fieldnames=fields)
                writer.writeheader()
                writer.writerows(combined_rows)
            print(out_csv)


if __name__ == "__main__":
    main()
