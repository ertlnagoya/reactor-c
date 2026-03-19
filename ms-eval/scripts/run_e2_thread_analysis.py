#!/usr/bin/env python3
import argparse
import csv
import re
import subprocess
from pathlib import Path

ROOT = Path("/Users/yutaka/program/reactor-c")


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


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repeats", type=int, default=30)
    ap.add_argument("--load", type=float, default=1.15)
    ap.add_argument("--steps", type=int, default=1600)
    ap.add_argument("--workers", type=int, default=2)
    ap.add_argument("--hc-workers", type=int, default=1)
    ap.add_argument("--hc", type=int, default=1)
    ap.add_argument("--lc", type=int, default=5)
    ap.add_argument("--hc-work-us", type=int, default=90)
    ap.add_argument("--lc-work-us", type=int, default=230)
    ap.add_argument("--deadline-us", type=int, default=1800)
    ap.add_argument("--stress-cpu", type=int, default=1)
    ap.add_argument("--stress-load", type=int, default=80)
    ap.add_argument("--stress-timeout-s", type=int, default=360)
    ap.add_argument("--stress-warmup-s", type=int, default=3)
    ap.add_argument("--container-cpuset", default="0-3")
    ap.add_argument("--lf-cpu-set", default="0-1")
    ap.add_argument("--stress-cpu-set", default="2")
    ap.add_argument("--drop-initial-tags", type=int, default=20)
    ap.add_argument("--inter-arm-sleep-ms", type=int, default=200)
    ap.add_argument("--os-lag-ns", type=int, default=300000)
    ap.add_argument("--os-ready-q-len", type=int, default=4)
    ap.add_argument("--os-lc-base-nice-delta", type=int, default=0)
    ap.add_argument("--os-nice-delta", type=int, default=2)
    ap.add_argument("--os-hc-nice-delta", type=int, default=0)
    ap.add_argument("--os-rt-enable", type=int, default=1)
    ap.add_argument("--os-rt-prio-hc", type=int, default=10)
    ap.add_argument("--os-rt-prio-lc", type=int, default=2)
    ap.add_argument("--hc-guard-enable", type=int, default=1)
    ap.add_argument("--hc-guard-lag-ns", type=int, default=300000)
    ap.add_argument("--hc-guard-ready-q-len", type=int, default=4)
    ap.add_argument("--docker-cap-sys-nice", type=int, default=1)
    ap.add_argument("--out-prefix", default="e2_thread_analysis")
    args = ap.parse_args()

    log_root = ROOT / "ms-eval/logs/e2_thread_analysis"
    log_root.mkdir(parents=True, exist_ok=True)
    rows = []
    skip_compile = 0

    conditions = [
        ("baseline", 0),
        ("rt_single_worker", 0),
        ("rt_worker_group", 1),
    ]

    for rep in range(1, args.repeats + 1):
        for condition, rt_group_enable in conditions:
            run_dir = log_root / f"{args.out_prefix}" / condition / f"rep{rep:02d}"
            run_dir.mkdir(parents=True, exist_ok=True)
            pidstat_out = run_dir / "pidstat.log"
            pidstat_out_in_container = Path("/workspace/reactor-c") / pidstat_out.relative_to(ROOT)

            if condition == "baseline":
                env_prefix = (
                    "LF_MS_OS_ENABLE=0 LF_MS_WORKER_PARTITION_ENABLE=0 LF_MS_HC_WORKERS=0 "
                    "LF_MS_MINIMAL_LOG=0 LF_MS_REPORT_MIN_INTERVAL_NS=0 MS_EVAL_LOG_REACTION_START=0"
                )
            else:
                env_prefix = (
                    "LF_MS_OS_ENABLE=1 "
                    f"LF_MS_OS_LAG_NS={args.os_lag_ns} "
                    f"LF_MS_OS_READY_Q_LEN={args.os_ready_q_len} "
                    f"LF_MS_OS_LC_BASE_NICE_DELTA={args.os_lc_base_nice_delta} "
                    f"LF_MS_OS_NICE_DELTA={args.os_nice_delta} "
                    f"LF_MS_OS_HC_NICE_DELTA={args.os_hc_nice_delta} "
                    f"LF_MS_OS_RT_ENABLE={args.os_rt_enable} "
                    f"LF_MS_OS_RT_GROUP_ENABLE={rt_group_enable} "
                    f"LF_MS_OS_RT_PRIO_HC={args.os_rt_prio_hc} "
                    f"LF_MS_OS_RT_PRIO_LC={args.os_rt_prio_lc} "
                    f"LF_MS_HC_GUARD_ENABLE={args.hc_guard_enable} "
                    f"LF_MS_HC_GUARD_LAG_NS={args.hc_guard_lag_ns} "
                    f"LF_MS_HC_GUARD_READY_Q_LEN={args.hc_guard_ready_q_len} "
                    f"LF_MS_HC_WORKERS={args.hc_workers} "
                    "LF_MS_WORKER_PARTITION_ENABLE=1 "
                    "LF_MS_MINIMAL_LOG=0 LF_MS_REPORT_MIN_INTERVAL_NS=0 MS_EVAL_LOG_REACTION_START=0"
                )

            run_cmd_inner = (
                f"env {env_prefix} "
                f"./run_e3.sh --workers {args.workers} --hc {args.hc} --lc {args.lc} "
                f"--steps {args.steps} --hc-work-us {args.hc_work_us} --lc-work-us {args.lc_work_us} "
                f"--hc-deadline-us {args.deadline_us} --lc-deadline-us {args.deadline_us} "
                f"--period-us 1000 --load-factors {args.load:.2f} "
                f"--stress-cpu 0 --stress-load 0 --stress-timeout-s 1 "
                f"--lf-cpu-set {args.lf_cpu_set} --skip-compile {skip_compile}"
            )
            inner = (
                "set -e; "
                f"stress-ng --cpu {args.stress_cpu} --cpu-load {args.stress_load} "
                f"--timeout {args.stress_timeout_s}s --taskset {args.stress_cpu_set} "
                f">/tmp/stress_e2_thread.log 2>&1 & SP=\\$!; "
                f"sleep {args.stress_warmup_s}; "
                f"mkdir -p '{pidstat_out_in_container.parent}'; "
                f"pidstat -t -u -w -h -p ALL 1 > '{pidstat_out_in_container}' 2>&1 & PP=\\$!; "
                f"{run_cmd_inner}; "
                "kill \\$PP >/dev/null 2>&1 || true; wait \\$PP >/dev/null 2>&1 || true; "
                "kill \\$SP >/dev/null 2>&1 || true; wait \\$SP >/dev/null 2>&1 || true; "
            )
            docker_cap = "--cap-add SYS_NICE " if args.docker_cap_sys_nice else ""
            cmd = (
                "docker run --rm -t "
                f"{docker_cap}"
                f"--cpuset-cpus {args.container_cpuset} "
                f"-v {ROOT}:/workspace/reactor-c "
                "-w /workspace/reactor-c lf-ms-phase4:latest "
                f"bash -lc \"{inner}\""
            )
            out = run_cmd(cmd)
            ts = latest_ts(out)
            rows.append([rep, condition, ts, args.load, args.drop_initial_tags, str(pidstat_out)])
            skip_compile = 1
            print(f"rep {rep}/{args.repeats} {condition} ts={ts}")

    manifest = ROOT / f"ms-eval/results/{args.out_prefix}_manifest.csv"
    manifest.parent.mkdir(parents=True, exist_ok=True)
    with manifest.open("w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["rep", "condition", "ts", "load", "drop_initial_tags", "pidstat_log"])
        writer.writerows(rows)
    print(manifest)


if __name__ == "__main__":
    main()
