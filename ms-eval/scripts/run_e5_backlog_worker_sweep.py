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
BACKLOG = SCRIPT_DIR / "summarize_e3_backlog.py"


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


def read_timestamps(long_csv: Path) -> list[tuple[int, str]]:
    seen = []
    used = set()
    with long_csv.open() as f:
        for row in csv.DictReader(f):
            key = (int(row["rep"]), row["ts"])
            if key not in used:
                used.add(key)
                seen.append(key)
    return seen


def append_backlog_rows(backlog_csv: Path, out_rows: list[dict[str, str]], worker: int, rep: int):
    with backlog_csv.open() as f:
        for row in csv.DictReader(f):
            row = dict(row)
            row["worker_setting"] = str(worker)
            row["rep"] = str(rep)
            out_rows.append(row)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--workers", default="1,2,4")
    ap.add_argument("--loads", default="0.4,0.8,1.2,1.6")
    ap.add_argument("--repeats", type=int, default=20)
    ap.add_argument("--steps", type=int, default=300)
    ap.add_argument("--hc", type=int, default=2)
    ap.add_argument("--lc", type=int, default=6)
    ap.add_argument("--lc-budget", type=int, default=4)
    ap.add_argument("--hc-work-us", type=int, default=180)
    ap.add_argument("--lc-work-us", type=int, default=220)
    ap.add_argument("--deadline-us", type=int, default=900)
    ap.add_argument("--period-us", type=int, default=1000)
    ap.add_argument("--degrade-lag-ns", type=int, default=150000)
    ap.add_argument("--degrade-ready-q-len", type=int, default=2)
    ap.add_argument("--drop-initial-tags", type=int, default=20)
    ap.add_argument("--cmake-bin-dir", default="/Users/yutaka/Library/Python/3.9/bin")
    ap.add_argument("--out-prefix", default="tcrs_e5_backlog")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    workers = [int(x.strip()) for x in args.workers.split(",") if x.strip()]
    out_dir = DATA / "e5"
    out_dir.mkdir(parents=True, exist_ok=True)
    combined_rows: list[dict[str, str]] = []

    for worker in workers:
        prefix = f"{args.out_prefix}_w{worker}"
        cmd = [
            "python3", str(RUN_COMPARE),
            "--workers", str(worker),
            "--loads", args.loads,
            "--repeats", str(args.repeats),
            "--steps", str(args.steps),
            "--hc", str(args.hc),
            "--lc", str(args.lc),
            "--lc-budget", str(args.lc_budget),
            "--hc-work-us", str(args.hc_work_us),
            "--lc-work-us", str(args.lc_work_us),
            "--deadline-us", str(args.deadline_us),
            "--period-us", str(args.period_us),
            "--degrade-lag-ns", str(args.degrade_lag_ns),
            "--degrade-ready-q-len", str(args.degrade_ready_q_len),
            "--out-prefix", prefix,
        ]
        run(cmd, REACTOR_C, env_path=args.cmake_bin_dir, dry_run=args.dry_run)

        long_csv = REACTOR_C / f"ms-eval/results/{prefix}_long.csv"
        summary_csv = REACTOR_C / f"ms-eval/results/{prefix}_summary.csv"
        if args.dry_run:
            continue
        if not long_csv.exists():
            raise FileNotFoundError(long_csv)

        (out_dir / long_csv.name).write_bytes(long_csv.read_bytes())
        if summary_csv.exists():
            (out_dir / summary_csv.name).write_bytes(summary_csv.read_bytes())

        for rep, ts in read_timestamps(long_csv):
            backlog_csv = out_dir / f"{prefix}_rep{rep}_backlog.csv"
            run([
                "python3", str(BACKLOG),
                "--logs", str(REACTOR_C / f"ms-eval/logs/e3/{ts}"),
                "--out", str(backlog_csv),
                "--period-us", str(args.period_us),
                "--drop-initial-tags", str(args.drop_initial_tags),
            ], REACTOR_C, dry_run=False)
            append_backlog_rows(backlog_csv, combined_rows, worker, rep)

    if not args.dry_run:
        out_csv = out_dir / f"{args.out_prefix}_combined_backlog.csv"
        fields = [
            "worker_setting", "rep", "timestamp", "mode", "load_factor", "workers",
            "observed_tags", "tag_completion_mean_us", "tag_completion_max_us",
            "hc_tag_completion_mean_us", "tag_overrun_ratio", "longest_overrun_run",
        ]
        with out_csv.open("w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=fields)
            writer.writeheader()
            writer.writerows(combined_rows)
        print(out_csv)


if __name__ == "__main__":
    main()
