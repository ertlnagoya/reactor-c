#!/usr/bin/env python3
import argparse
import csv
import math
import random
import re
import subprocess
from pathlib import Path
from statistics import mean, stdev

ROOT = Path('/Users/yutaka/program/reactor-c')
PARSE_E3 = ROOT / 'ms-eval/scripts/parse_e3.py'


def run_cmd(cmd: str) -> str:
    p = subprocess.run(cmd, shell=True, text=True, capture_output=True)
    if p.returncode != 0:
        raise RuntimeError(f'command failed: {cmd}\nstdout:\n{p.stdout}\nstderr:\n{p.stderr}')
    return p.stdout + p.stderr


def extract_run_timestamps(output: str) -> list[str]:
    ts = re.findall(r'E3 logs written under .*?/e3/(\d{8}_\d{6})', output)
    if len(ts) < 2:
        raise RuntimeError('failed to extract paired timestamps from output')
    return ts[-2:]


def parse_pair(ts: str, load: float, drop_initial_tags: int) -> tuple[float, float]:
    tmp_csv = ROOT / f'ms-eval/results/_tmp_pair_{ts}.csv'
    run_cmd(
        f'python3 {PARSE_E3} '
        f'--logs {ROOT}/ms-eval/logs/e3/{ts} '
        f'--drop-initial-tags {drop_initial_tags} '
        f'--out {tmp_csv}'
    )
    baseline = None
    degrade = None
    with tmp_csv.open() as f:
        r = csv.DictReader(f)
        for row in r:
            if float(row['load_factor']) != load:
                continue
            if row['mode'] == 'baseline':
                baseline = float(row['hc_miss_rate'])
            elif row['mode'] == 'degrade':
                degrade = float(row['hc_miss_rate'])
    tmp_csv.unlink(missing_ok=True)
    if baseline is None or degrade is None:
        raise RuntimeError(f'missing baseline/degrade for ts={ts}, load={load}')
    return baseline, degrade


def os_apply_count(ts: str, load: float) -> int:
    ms = ROOT / f'ms-eval/logs/e3/{ts}/degrade/L{load}/ms.log'
    txt = ms.read_text(errors='ignore') if ms.exists() else ''
    return txt.count('event=os_policy_apply')


def ci95(vals: list[float]) -> tuple[float, float]:
    if not vals:
        return (math.nan, math.nan)
    if len(vals) == 1:
        return (vals[0], vals[0])
    m = mean(vals)
    s = stdev(vals)
    half = 1.96 * s / math.sqrt(len(vals))
    return (m - half, m + half)


def permutation_pvalue(a: list[float], b: list[float], iters: int = 20000) -> float:
    if not a or not b:
        return math.nan
    obs = abs(mean(a) - mean(b))
    pool = a + b
    n = len(a)
    cnt = 0
    for _ in range(iters):
        random.shuffle(pool)
        d = abs(mean(pool[:n]) - mean(pool[n:]))
        if d >= obs:
            cnt += 1
    return (cnt + 1) / (iters + 1)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument('--repeats', type=int, default=30)
    ap.add_argument('--load', type=float, default=1.2)
    ap.add_argument('--steps', type=int, default=2000)
    ap.add_argument('--workers', type=int, default=2)
    ap.add_argument('--hc-workers', type=int, default=1)
    ap.add_argument('--hc', type=int, default=1)
    ap.add_argument('--lc', type=int, default=5)
    ap.add_argument('--hc-work-us', type=int, default=90)
    ap.add_argument('--lc-work-us', type=int, default=340)
    ap.add_argument('--deadline-us', type=int, default=2000)
    ap.add_argument('--stress-cpu', type=int, default=1)
    ap.add_argument('--stress-load', type=int, default=70)
    ap.add_argument('--stress-timeout-s', type=int, default=360)
    ap.add_argument('--stress-warmup-s', type=int, default=2)
    ap.add_argument('--container-cpuset', default='0-3')
    ap.add_argument('--lf-cpu-set', default='0-1')
    ap.add_argument('--stress-cpu-set', default='2')
    ap.add_argument('--drop-initial-tags', type=int, default=50)
    ap.add_argument('--alternate-order', type=int, default=1)
    ap.add_argument('--inter-arm-sleep-ms', type=int, default=50)
    ap.add_argument('--os-lag-ns', type=int, default=200000)
    ap.add_argument('--os-ready-q-len', type=int, default=2)
    ap.add_argument('--os-lc-base-nice-delta', type=int, default=0)
    ap.add_argument('--os-nice-delta', type=int, default=10)
    ap.add_argument('--os-hc-nice-delta', type=int, default=0)
    ap.add_argument('--os-rt-enable', type=int, default=0)
    ap.add_argument('--os-rt-group-enable', type=int, default=0)
    ap.add_argument('--os-rt-prio-hc', type=int, default=10)
    ap.add_argument('--os-rt-prio-lc', type=int, default=2)
    ap.add_argument('--os-min-switch-ns', type=int, default=20000000)
    ap.add_argument('--hc-guard-enable', type=int, default=0)
    ap.add_argument('--hc-guard-lag-ns', type=int, default=-1)
    ap.add_argument('--hc-guard-ready-q-len', type=int, default=-1)
    ap.add_argument('--out-prefix', default='e1_paired')
    args = ap.parse_args()

    load_s = f'{args.load:.2f}'
    rows = []
    skip_compile = 0

    for rep in range(1, args.repeats + 1):
        run_policy_env = (
            'LF_MS_OS_ENABLE=0 LF_MS_WORKER_PARTITION_ENABLE=0 LF_MS_HC_WORKERS=0 '
            'LF_MS_MINIMAL_LOG=1 MS_EVAL_LOG_REACTION_START=0'
        )
        run_policy_os_env = (
            'LF_MS_OS_ENABLE=1 '
            f'LF_MS_OS_LAG_NS={args.os_lag_ns} '
            f'LF_MS_OS_READY_Q_LEN={args.os_ready_q_len} '
            f'LF_MS_OS_LC_BASE_NICE_DELTA={args.os_lc_base_nice_delta} '
            f'LF_MS_OS_NICE_DELTA={args.os_nice_delta} '
            f'LF_MS_OS_HC_NICE_DELTA={args.os_hc_nice_delta} '
            f'LF_MS_OS_RT_ENABLE={args.os_rt_enable} '
            f'LF_MS_OS_RT_GROUP_ENABLE={args.os_rt_group_enable} '
            f'LF_MS_OS_RT_PRIO_HC={args.os_rt_prio_hc} '
            f'LF_MS_OS_RT_PRIO_LC={args.os_rt_prio_lc} '
            f'LF_MS_OS_MIN_SWITCH_NS={args.os_min_switch_ns} '
            f'LF_MS_HC_GUARD_ENABLE={args.hc_guard_enable} '
            f'LF_MS_HC_GUARD_LAG_NS={args.hc_guard_lag_ns} '
            f'LF_MS_HC_GUARD_READY_Q_LEN={args.hc_guard_ready_q_len} '
            'LF_MS_WORKER_PARTITION_ENABLE=1 '
            f'LF_MS_HC_WORKERS={args.hc_workers} '
            'LF_MS_MINIMAL_LOG=1 MS_EVAL_LOG_REACTION_START=0'
        )
        run_policy_cmd = (
            f'env {run_policy_env} '
            f'./run_e3.sh --workers {args.workers} --hc {args.hc} --lc {args.lc} '
            f'--steps {args.steps} --hc-work-us {args.hc_work_us} --lc-work-us {args.lc_work_us} '
            f'--hc-deadline-us {args.deadline_us} --lc-deadline-us {args.deadline_us} '
            f'--period-us 1000 --load-factors {load_s} '
            '--stress-cpu 0 --stress-load 0 --stress-timeout-s 1 '
            f'--lf-cpu-set {args.lf_cpu_set} --skip-compile {skip_compile}; '
        )
        run_policy_os_cmd = (
            f'env {run_policy_os_env} '
            f'./run_e3.sh --workers {args.workers} --hc {args.hc} --lc {args.lc} '
            f'--steps {args.steps} --hc-work-us {args.hc_work_us} --lc-work-us {args.lc_work_us} '
            f'--hc-deadline-us {args.deadline_us} --lc-deadline-us {args.deadline_us} '
            f'--period-us 1000 --load-factors {load_s} '
            '--stress-cpu 0 --stress-load 0 --stress-timeout-s 1 '
            f'--lf-cpu-set {args.lf_cpu_set} --skip-compile 1; '
        )
        if args.alternate_order and (rep % 2 == 0):
            arm1_label, arm1_cmd = 'policy_os', run_policy_os_cmd
            arm2_label, arm2_cmd = 'policy', run_policy_cmd
        else:
            arm1_label, arm1_cmd = 'policy', run_policy_cmd
            arm2_label, arm2_cmd = 'policy_os', run_policy_os_cmd

        script = ''.join([
            'set -e; ',
            f'stress-ng --cpu {args.stress_cpu} --cpu-load {args.stress_load} ',
            f'--timeout {args.stress_timeout_s}s ',
            f'--taskset {args.stress_cpu_set} >/tmp/stress_pair.log 2>&1 & ',
            'SP=\\$!; ',
            f'sleep {args.stress_warmup_s}; ',
            arm1_cmd,
            f'sleep {args.inter_arm_sleep_ms}e-3; ',
            arm2_cmd,
            'kill \\$SP >/dev/null 2>&1 || true; wait \\$SP >/dev/null 2>&1 || true; ',
        ])
        cmd = (
            'docker run --rm -t '
            f'--cpuset-cpus {args.container_cpuset} '
            f'-v {ROOT}:/workspace/reactor-c '
            '-w /workspace/reactor-c lf-ms-phase4:latest '
            f'bash -lc "{script}"'
        )
        out = run_cmd(cmd)
        ts0, ts1 = extract_run_timestamps(out)
        arm_ts = {arm1_label: ts0, arm2_label: ts1}
        policy_ts = arm_ts['policy']
        policy_os_ts = arm_ts['policy_os']

        b_p, d_p = parse_pair(policy_ts, args.load, args.drop_initial_tags)
        b_o, d_o = parse_pair(policy_os_ts, args.load, args.drop_initial_tags)
        ap_p = os_apply_count(policy_ts, args.load)
        ap_o = os_apply_count(policy_os_ts, args.load)

        rows.append({
            'rep': rep,
            'policy_ts': policy_ts,
            'policy_os_ts': policy_os_ts,
            'baseline_policy': b_p,
            'baseline_policy_os': b_o,
            'policy': d_p,
            'policy_os': d_o,
            'delta_raw': d_o - d_p,
            'policy_minus_base': d_p - b_p,
            'policy_os_minus_base': d_o - b_o,
            'delta_norm': (d_o - b_o) - (d_p - b_p),
            'policy_os_apply': ap_p,
            'policy_os_os_apply': ap_o,
        })
        skip_compile = 1
        print(f'rep {rep}/{args.repeats} policy={d_p:.3f} policy_os={d_o:.3f}')

    res_dir = ROOT / 'ms-eval/results'
    res_dir.mkdir(parents=True, exist_ok=True)

    long_csv = res_dir / f'{args.out_prefix}_long.csv'
    with long_csv.open('w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)

    b = [r['baseline_policy'] for r in rows]
    bo = [r['baseline_policy_os'] for r in rows]
    p = [r['policy'] for r in rows]
    o = [r['policy_os'] for r in rows]
    d = [r['delta_raw'] for r in rows]
    pb = [r['policy_minus_base'] for r in rows]
    ob = [r['policy_os_minus_base'] for r in rows]
    dn = [r['delta_norm'] for r in rows]

    summary = {
        'n': len(rows),
        'baseline_policy_mean': mean(b),
        'baseline_policy_ci_low': ci95(b)[0],
        'baseline_policy_ci_high': ci95(b)[1],
        'baseline_policy_os_mean': mean(bo),
        'baseline_policy_os_ci_low': ci95(bo)[0],
        'baseline_policy_os_ci_high': ci95(bo)[1],
        'policy_mean': mean(p),
        'policy_ci_low': ci95(p)[0],
        'policy_ci_high': ci95(p)[1],
        'policy_os_mean': mean(o),
        'policy_os_ci_low': ci95(o)[0],
        'policy_os_ci_high': ci95(o)[1],
        'delta_raw_mean': mean(d),
        'delta_raw_ci_low': ci95(d)[0],
        'delta_raw_ci_high': ci95(d)[1],
        'delta_norm_mean': mean(dn),
        'delta_norm_ci_low': ci95(dn)[0],
        'delta_norm_ci_high': ci95(dn)[1],
        'p_value_raw_policy_os_vs_policy': permutation_pvalue(o, p),
        'p_value_norm_policy_os_vs_policy': permutation_pvalue(ob, pb),
        'policy_os_apply_mean': mean([r['policy_os_apply'] for r in rows]),
        'policy_os_os_apply_mean': mean([r['policy_os_os_apply'] for r in rows]),
    }

    summary_csv = res_dir / f'{args.out_prefix}_summary.csv'
    with summary_csv.open('w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=list(summary.keys()))
        w.writeheader()
        w.writerow(summary)

    print(long_csv)
    print(summary_csv)


if __name__ == '__main__':
    main()
