# MS Evaluation Suite (E1/E2/E3)

This folder contains a reproducible evaluation harness for the paper
“Requirements-Driven Design of a User-Space Master Scheduler for Deterministic Reactive CPS”.

It generates LF benchmarks, runs experiments E1/E2/E3, parses logs to CSV, and produces plots.

## Quick start
From repo root:

```bash
./run_all.sh
```

Outputs:
- `ms-eval/results/e1_overhead.csv`
- `ms-eval/results/e2_correctness.csv`
- `ms-eval/results/e2_miss_detection.csv`
- `ms-eval/results/e3_missrate.csv`
- `ms-eval/figures/fig2_overhead.pdf`
- `ms-eval/figures/fig3_missrate.pdf`
- `ms-eval/results/metadata.json`

## Requirements
- Linux (x86_64)
- `lfc` in PATH (or set `LFC=/path/to/lfc`)
- `python3` + `matplotlib`

## Experiments

### E1: Overhead (observation vs intervention)

```bash
./run_e1.sh --N 16,32,64,128 --iterations 1000 --workload-us 50 --deadline-us 200 --period-us 1000 --seed 1
```

Modes:
- `baseline`: `LF_MS_DISABLE=1`
- `observe`: `LF_MS_OBSERVE_ONLY=1` (log-only, no intervention)
- `intervene`: MS pick-next enforced

### E2: Semantic correctness + miss detection

```bash
./run_e2.sh --N 64 --iterations 500 --workload-us 50 --deadline-us 200 \
  --inject-period 10 --inject-rate-pct 30 --inject-delay-us 500
```

- Correctness uses MS `ready` + `pick_next` events for membership/ordering checks.
- Miss detection compares injected delays to `reaction_end.missed_deadline`.

### E3: Controlled degradation under overload

```bash
./run_e3.sh --hc 8 --lc 8 --hc-work-us 80 --lc-work-us 60 \
  --load-factors 1.0,1.5,2.0,2.5 --lc-budget 4 --window-ns 1000000
```

- Baseline: `LF_MS_DISABLE=1`
- Degrade: MS policy config generated per run (`ms_policy.cfg`)

## Logging
- App logs: JSONL (`LF_APP_LOG`)
- MS logs: key-value (`LF_MS_LOG`)

See `ms-eval/LOG_FORMAT.md` for details.

## Notes / TODOs
- `LF_MS_OBSERVE_ONLY` is added in `core/utils/master_scheduler.c` to support observe-only mode.
- If your MS hooks emit explicit deadline-miss events, update `scripts/parse_e2.py` to use them
  for detection instead of `reaction_end.missed_deadline`.
- The MS config generator assumes reaction indices match the generator order.
  If your generated indices differ, add a mapping step in the parser or config generator.

## Structure
```
ms-eval/
  lf-templates/        # C logging helpers
  lf-gen/              # generated LF programs
  scripts/             # generators, runners, parsers, plots
  logs/                # raw logs
  results/             # CSV + metadata
  figures/             # PDF plots
```
