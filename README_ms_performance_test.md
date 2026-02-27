# MS Evaluation Test Guide (E1 / E2)

This document describes reproducible steps for the current MS performance evaluation:

- E1: runtime overhead of MS intervention
- E2: HC miss-rate comparison across LF baseline and two MS+RT configurations

## 1. Common workflow (Docker rebuild and shell)

Run all evaluation commands from the repository root (`/workspace/reactor-c` in container).

```bash
docker build --no-cache -t lf-ms-phase4 .
docker run --rm -it \
  -v "$PWD":/workspace/reactor-c \
  -w /workspace/reactor-c \
  lf-ms-phase4 \
  bash
```

Inside the container, verify:

```bash
pwd
ls
```

## 2. E1: MS overhead measurement

### Objective

Measure step-time overhead when MS intervention is enabled (`intervene`) against LF baseline (`baseline`).

### Test conditions

- Microbench sizes: `N=16,32,64,128`
- Steps: `1000`
- Reaction workload: `50 us`
- Relative deadline: `200 us`
- Period: `1000 us`
- Modes per N:
  - `baseline`: `LF_MS_DISABLE=1`
  - `observe`: `LF_MS_DISABLE=0, LF_MS_OBSERVE_ONLY=1`
  - `intervene`: `LF_MS_DISABLE=0, LF_MS_OBSERVE_ONLY=0`

### Run

```bash
./run_e1.sh
```

Expected tail output:

```text
E1 logs written under /workspace/reactor-c/ms-eval/logs/e1/<timestamp>
```

### Parse and plot

Replace `<timestamp>` with the value printed above.

```bash
python3 ms-eval/scripts/parse_e1.py \
  --logs ms-eval/logs/e1/<timestamp> \
  --out ms-eval/results/e1_overhead_<timestamp>.csv

python3 ms-eval/scripts/plot_fig2.py \
  --in ms-eval/results/e1_overhead_<timestamp>.csv \
  --out ms-eval/figures/fig2_overhead_<timestamp>.png
```

Quick check:

```bash
column -s, -t < ms-eval/results/e1_overhead_<timestamp>.csv
```

Interpretation:

- `overhead_pct` is computed against the same `(N, workload_us)` baseline row.
- Main comparison for paper text is `intervene` vs `baseline`.

## 3. E2: HC miss-rate comparison (baseline vs RT single-worker vs RT worker-group)

### Objective

Compare HC miss rate under the same stress condition:

1. LF baseline (MS disabled path, pooled from both runs)
2. MS + RT (single-worker)
3. MS + RT (worker-group, proposed)

### Reproducibility-critical settings

The E2 scripts set and use the following control knobs:

- `LF_MS_OS_ENABLE`
- `LF_MS_OS_RT_ENABLE`
- `LF_MS_OS_RT_GROUP_ENABLE`
- `LF_MS_OS_RT_PRIO_HC`
- `LF_MS_OS_RT_PRIO_LC`
- `LF_MS_OS_HC_NICE_DELTA`
- `LF_MS_OS_LC_BASE_NICE_DELTA`
- `LF_MS_HC_GUARD_ENABLE`
- `LF_MS_HC_GUARD_LAG_NS`
- `LF_MS_HC_GUARD_READY_Q_LEN`
- `LF_MS_MINIMAL_LOG=1`
- `MS_EVAL_LOG_REACTION_START=0` (reduce logging overhead)

### Common E2 workload/stress parameters

- `repeats=30`
- `load=1.15`
- `steps=1600`
- `workers=2`, `hc-workers=1`
- `hc=1`, `lc=5`
- `hc-work-us=90`, `lc-work-us=230`
- `deadline-us=1800`
- `stress-ng`: `--cpu 1 --cpu-load 80 --timeout 360s --taskset 2`
- CPU sets: container `0-3`, LF `0-1`
- `drop-initial-tags=20`
- Docker capability for RT/nice operation: `--cap-add SYS_NICE`

### 3.1 Comparison arm: RT single-worker

```bash
python3 ms-eval/scripts/run_e1_triplet_compare.py \
  --repeats 30 \
  --load 1.15 \
  --steps 1600 \
  --workers 2 \
  --hc-workers 1 \
  --hc 1 \
  --lc 5 \
  --hc-work-us 90 \
  --lc-work-us 230 \
  --deadline-us 1800 \
  --stress-cpu 1 \
  --stress-load 80 \
  --stress-timeout-s 360 \
  --stress-warmup-s 3 \
  --container-cpuset 0-3 \
  --lf-cpu-set 0-1 \
  --stress-cpu-set 2 \
  --drop-initial-tags 20 \
  --inter-arm-sleep-ms 200 \
  --os-lag-ns 300000 \
  --os-ready-q-len 4 \
  --os-lc-base-nice-delta 0 \
  --os-nice-delta 2 \
  --os-hc-nice-delta 0 \
  --os-rt-enable 1 \
  --os-rt-group-enable 0 \
  --os-rt-prio-hc 10 \
  --os-rt-prio-lc 2 \
  --hc-guard-enable 1 \
  --hc-guard-lag-ns 300000 \
  --hc-guard-ready-q-len 4 \
  --docker-cap-sys-nice 1 \
  --out-prefix e1_rtaxis_fix_rt_guard_r30
```

Artifacts:

- `ms-eval/results/e1_rtaxis_fix_rt_guard_r30_long.csv`
- `ms-eval/results/e1_rtaxis_fix_rt_guard_r30_summary.csv`

### 3.2 Proposed arm: RT worker-group

```bash
python3 ms-eval/scripts/run_e1_triplet_compare.py \
  --repeats 30 \
  --load 1.15 \
  --steps 1600 \
  --workers 2 \
  --hc-workers 1 \
  --hc 1 \
  --lc 5 \
  --hc-work-us 90 \
  --lc-work-us 230 \
  --deadline-us 1800 \
  --stress-cpu 1 \
  --stress-load 80 \
  --stress-timeout-s 360 \
  --stress-warmup-s 3 \
  --container-cpuset 0-3 \
  --lf-cpu-set 0-1 \
  --stress-cpu-set 2 \
  --drop-initial-tags 20 \
  --inter-arm-sleep-ms 200 \
  --os-lag-ns 300000 \
  --os-ready-q-len 4 \
  --os-lc-base-nice-delta 0 \
  --os-nice-delta 2 \
  --os-hc-nice-delta 0 \
  --os-rt-enable 1 \
  --os-rt-group-enable 1 \
  --os-rt-prio-hc 10 \
  --os-rt-prio-lc 2 \
  --hc-guard-enable 1 \
  --hc-guard-lag-ns 300000 \
  --hc-guard-ready-q-len 4 \
  --docker-cap-sys-nice 1 \
  --out-prefix e1_rtgroup_main_r30
```

Artifacts:

- `ms-eval/results/e1_rtgroup_main_r30_long.csv`
- `ms-eval/results/e1_rtgroup_main_r30_summary.csv`

### 3.3 E2 figure generation

```bash
python3 ms-eval/scripts/plot_e2_rt_comparison_v2.py \
  --comparison-long ms-eval/results/e1_rtaxis_fix_rt_guard_r30_long.csv \
  --proposed-long ms-eval/results/e1_rtgroup_main_r30_long.csv \
  --out-png ms-eval/figures/fig_e2_rt_comparison_v2.png \
  --out-pdf ms-eval/figures/fig_e2_rt_comparison_v2.pdf \
  --out-csv ms-eval/results/e2_rt_comparison_plotdata_v2.csv
```

Output labels in the plot:

- `LF baseline (MS disabled)`
- `MS + RT (single-worker)`
- `MS + RT (worker-group, proposed)`

Sample size note:

- baseline in the plot is pooled from both long CSVs (`n=60`)
- each RT condition uses one long CSV (`n=30`)

## 4. Practical notes for reproducibility

- Keep Docker image and host load conditions fixed while collecting data.
- `ms-eval/logs` can become very large; keep only required timestamps for archival.
- If `os_policy_apply` is unexpectedly zero, verify container capability (`--cap-add SYS_NICE`) and RT flags.
- For paper updates, always report:
  - command lines
  - exact CSV file names used
  - figure generation command
