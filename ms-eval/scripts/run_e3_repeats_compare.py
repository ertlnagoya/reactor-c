#!/usr/bin/env python3
import argparse
import csv
import json
import math
import random
import re
import subprocess
from dataclasses import dataclass
from pathlib import Path
from statistics import mean, stdev

ROOT = Path('/Users/yutaka/program/reactor-c')
PARSE_E3 = ROOT / 'ms-eval/scripts/parse_e3.py'


@dataclass
class RunResult:
    ts: str
    baseline: dict
    degrade: dict
    os_apply: dict


def run_cmd(cmd: str) -> str:
    p = subprocess.run(cmd, shell=True, text=True, capture_output=True)
    if p.returncode != 0:
        raise RuntimeError(f'command failed: {cmd}\nstdout:\n{p.stdout}\nstderr:\n{p.stderr}')
    return p.stdout + p.stderr


def latest_ts(output: str) -> str:
    matches = re.findall(r'\b\d{8}_\d{6}\b', output)
    if not matches:
        raise RuntimeError('timestamp not found in run output')
    return matches[-1]


def parse_csv(path: Path):
    baseline = {}
    degrade = {}
    with path.open() as f:
        r = csv.DictReader(f)
        for row in r:
            L = float(row['load_factor'])
            v = float(row['hc_miss_rate'])
            if row['mode'] == 'baseline':
                baseline[L] = v
            elif row['mode'] == 'degrade':
                degrade[L] = v
    return baseline, degrade


def os_apply_counts(ts: str, loads: list[float]) -> dict:
    out = {}
    for L in loads:
        ms = ROOT / f'ms-eval/logs/e3/{ts}/degrade/L{L}/ms.log'
        txt = ms.read_text(errors='ignore') if ms.exists() else ''
        out[L] = txt.count('event=os_policy_apply')
    return out


def run_one(mode: str, loads: list[float], steps: int, workers: int, hc_workers: int,
            hc: int, lc: int, hc_work: int, lc_work: int, deadline: int,
            stress_cpu: int, stress_load: int, stress_timeout_s: int,
            stress_warmup_s: int, lf_cpu_set: str, stress_cpu_set: str,
            container_cpuset: str, skip_compile: bool) -> RunResult:
    load_s = ','.join(str(x) for x in loads)
    part = 1 if mode == 'partitioned' else 0
    common = (
        'LF_MS_OS_ENABLE=1 LF_MS_OS_LAG_NS=50000 LF_MS_OS_NICE_DELTA=10 '
        'LF_MS_OS_MIN_SWITCH_NS=0 '
        f'LF_MS_WORKER_PARTITION_ENABLE={part} LF_MS_HC_WORKERS={hc_workers}'
    )
    run_e3_opts = (
        f'--stress-cpu {stress_cpu} --stress-load {stress_load} '
        f'--stress-timeout-s {stress_timeout_s} --stress-warmup-s {stress_warmup_s} '
        f'--skip-compile {1 if skip_compile else 0}'
    )
    if lf_cpu_set:
        run_e3_opts += f' --lf-cpu-set {lf_cpu_set}'
    if stress_cpu_set:
        run_e3_opts += f' --stress-cpu-set {stress_cpu_set}'
    docker_opts = ''
    if container_cpuset:
        docker_opts += f' --cpuset-cpus {container_cpuset}'
    cmd = (
        f'docker run --rm -t{docker_opts} '
        f'-v {ROOT}:/workspace/reactor-c '
        '-w /workspace/reactor-c lf-ms-phase4:latest '
        'bash -lc "set -e; '
        f'env {common} '
        f'./run_e3.sh --workers {workers} --hc {hc} --lc {lc} '
        f'--steps {steps} --hc-work-us {hc_work} --lc-work-us {lc_work} '
        f'--hc-deadline-us {deadline} --lc-deadline-us {deadline} '
        f'--period-us 1000 --load-factors {load_s} '
        f'{run_e3_opts}; '
        '"'
    )
    out = run_cmd(cmd)
    ts = latest_ts(out)

    tmp_csv = ROOT / f'ms-eval/results/_tmp_{mode}_{ts}.csv'
    run_cmd(f'python3 {PARSE_E3} --logs {ROOT}/ms-eval/logs/e3/{ts} --out {tmp_csv}')
    baseline, degrade = parse_csv(tmp_csv)
    tmp_csv.unlink(missing_ok=True)
    return RunResult(ts=ts, baseline=baseline, degrade=degrade, os_apply=os_apply_counts(ts, loads))


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
    ap.add_argument('--loads', default='0.4,1.0,1.6')
    ap.add_argument('--repeats', type=int, default=12)
    ap.add_argument('--steps', type=int, default=160)
    ap.add_argument('--workers', type=int, default=2)
    ap.add_argument('--hc-workers', type=int, default=1)
    ap.add_argument('--hc', type=int, default=4)
    ap.add_argument('--lc', type=int, default=8)
    ap.add_argument('--hc-work-us', type=int, default=40)
    ap.add_argument('--lc-work-us', type=int, default=120)
    ap.add_argument('--deadline-us', type=int, default=10000)
    ap.add_argument('--stress-cpu', type=int, default=1)
    ap.add_argument('--stress-load', type=int, default=80)
    ap.add_argument('--stress-timeout-s', type=int, default=360)
    ap.add_argument('--stress-warmup-s', type=int, default=3)
    ap.add_argument('--container-cpuset', default='0-3')
    ap.add_argument('--lf-cpu-set', default='0-1')
    ap.add_argument('--stress-cpu-set', default='2')
    ap.add_argument('--out-prefix', default='e3_repeat_compare')
    args = ap.parse_args()

    loads = [float(x.strip()) for x in args.loads.split(',') if x.strip()]
    long_rows = []
    compiled_once = False

    for i in range(1, args.repeats + 1):
        mixed = run_one('mixed', loads, args.steps, args.workers, 0,
                        args.hc, args.lc, args.hc_work_us, args.lc_work_us, args.deadline_us,
                        args.stress_cpu, args.stress_load, args.stress_timeout_s, args.stress_warmup_s,
                        args.lf_cpu_set, args.stress_cpu_set, args.container_cpuset,
                        compiled_once)
        compiled_once = True
        part = run_one('partitioned', loads, args.steps, args.workers, args.hc_workers,
                       args.hc, args.lc, args.hc_work_us, args.lc_work_us, args.deadline_us,
                       args.stress_cpu, args.stress_load, args.stress_timeout_s, args.stress_warmup_s,
                       args.lf_cpu_set, args.stress_cpu_set, args.container_cpuset,
                       compiled_once)

        for L in loads:
            mixed_base = mixed.baseline.get(L, math.nan)
            part_base = part.baseline.get(L, math.nan)
            mixed_deg = mixed.degrade.get(L, math.nan)
            part_deg = part.degrade.get(L, math.nan)
            mixed_vs_base = mixed_deg - mixed_base
            part_vs_base = part_deg - part_base
            long_rows.append([
                i, f'{L:.2f}',
                f'{mixed_base:.3f}',
                f'{part_base:.3f}',
                f'{mixed_deg:.3f}',
                f'{part_deg:.3f}',
                f'{part_deg - mixed_deg:.3f}',
                f'{mixed_vs_base:.3f}',
                f'{part_vs_base:.3f}',
                f'{part_vs_base - mixed_vs_base:.3f}',
                mixed.os_apply.get(L, 0),
                part.os_apply.get(L, 0),
                mixed.ts,
                part.ts,
            ])

    res_dir = ROOT / 'ms-eval/results'
    res_dir.mkdir(parents=True, exist_ok=True)
    long_csv = res_dir / f'{args.out_prefix}_long.csv'
    with long_csv.open('w', newline='') as f:
        w = csv.writer(f)
        w.writerow([
            'rep', 'load_factor',
            'mixed_baseline_hc_miss_rate', 'partitioned_baseline_hc_miss_rate',
            'mixed_hc_miss_rate', 'partitioned_hc_miss_rate',
            'delta_partition_minus_mixed',
            'mixed_minus_baseline', 'partitioned_minus_baseline',
            'delta_partition_minus_mixed_on_baseline_normalized',
            'mixed_os_apply', 'partitioned_os_apply', 'mixed_ts', 'partitioned_ts'
        ])
        w.writerows(long_rows)

    by_load = {
        L: {
            'baseline_mixed': [], 'baseline_part': [],
            'mixed': [], 'partitioned': [], 'delta': [],
            'mixed_vs_base': [], 'part_vs_base': [], 'delta_vs_base': [],
            'mixed_apply': [], 'part_apply': []
        } for L in loads
    }
    for r in long_rows:
        L = float(r[1])
        by_load[L]['baseline_mixed'].append(float(r[2]))
        by_load[L]['baseline_part'].append(float(r[3]))
        by_load[L]['mixed'].append(float(r[4]))
        by_load[L]['partitioned'].append(float(r[5]))
        by_load[L]['delta'].append(float(r[6]))
        by_load[L]['mixed_vs_base'].append(float(r[7]))
        by_load[L]['part_vs_base'].append(float(r[8]))
        by_load[L]['delta_vs_base'].append(float(r[9]))
        by_load[L]['mixed_apply'].append(float(r[10]))
        by_load[L]['part_apply'].append(float(r[11]))

    summary_csv = res_dir / f'{args.out_prefix}_summary.csv'
    with summary_csv.open('w', newline='') as f:
        w = csv.writer(f)
        w.writerow([
            'load_factor',
            'baseline_mixed_mean', 'baseline_mixed_ci_low', 'baseline_mixed_ci_high',
            'baseline_partitioned_mean', 'baseline_partitioned_ci_low', 'baseline_partitioned_ci_high',
            'mixed_mean', 'mixed_ci_low', 'mixed_ci_high',
            'partitioned_mean', 'partitioned_ci_low', 'partitioned_ci_high',
            'delta_mean', 'delta_ci_low', 'delta_ci_high',
            'mixed_vs_baseline_mean', 'mixed_vs_baseline_ci_low', 'mixed_vs_baseline_ci_high',
            'partitioned_vs_baseline_mean', 'partitioned_vs_baseline_ci_low', 'partitioned_vs_baseline_ci_high',
            'delta_vs_baseline_mean', 'delta_vs_baseline_ci_low', 'delta_vs_baseline_ci_high',
            'p_value_partition_vs_mixed',
            'p_value_delta_vs_baseline_partition_vs_mixed',
            'mixed_os_apply_mean', 'partitioned_os_apply_mean'
        ])
        for L in sorted(loads):
            b_m = by_load[L]['baseline_mixed']
            b_p = by_load[L]['baseline_part']
            m = by_load[L]['mixed']
            p = by_load[L]['partitioned']
            d = by_load[L]['delta']
            mvb = by_load[L]['mixed_vs_base']
            pvb = by_load[L]['part_vs_base']
            dvb = by_load[L]['delta_vs_base']
            b_m_lo, b_m_hi = ci95(b_m)
            b_p_lo, b_p_hi = ci95(b_p)
            m_lo, m_hi = ci95(m)
            p_lo, p_hi = ci95(p)
            d_lo, d_hi = ci95(d)
            mvb_lo, mvb_hi = ci95(mvb)
            pvb_lo, pvb_hi = ci95(pvb)
            dvb_lo, dvb_hi = ci95(dvb)
            pv = permutation_pvalue(p, m)
            pv_dvb = permutation_pvalue(pvb, mvb)
            w.writerow([
                f'{L:.2f}',
                f'{mean(b_m):.3f}', f'{b_m_lo:.3f}', f'{b_m_hi:.3f}',
                f'{mean(b_p):.3f}', f'{b_p_lo:.3f}', f'{b_p_hi:.3f}',
                f'{mean(m):.3f}', f'{m_lo:.3f}', f'{m_hi:.3f}',
                f'{mean(p):.3f}', f'{p_lo:.3f}', f'{p_hi:.3f}',
                f'{mean(d):.3f}', f'{d_lo:.3f}', f'{d_hi:.3f}',
                f'{mean(mvb):.3f}', f'{mvb_lo:.3f}', f'{mvb_hi:.3f}',
                f'{mean(pvb):.3f}', f'{pvb_lo:.3f}', f'{pvb_hi:.3f}',
                f'{mean(dvb):.3f}', f'{dvb_lo:.3f}', f'{dvb_hi:.3f}',
                f'{pv:.4f}',
                f'{pv_dvb:.4f}',
                f'{mean(by_load[L]["mixed_apply"]):.3f}',
                f'{mean(by_load[L]["part_apply"]):.3f}',
            ])

    print(long_csv)
    print(summary_csv)


if __name__ == '__main__':
    main()
