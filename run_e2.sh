#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EVAL_DIR="${ROOT_DIR}/ms-eval"
source "${EVAL_DIR}/scripts/run_common.sh"

N=64
iterations=500
workload_us=50
deadline_us=200
period_us=1000
seed=1
inject_period=10
inject_rate_pct=30
inject_delay_us=500

while [[ $# -gt 0 ]]; do
  case "$1" in
    --N) N="$2"; shift 2;;
    --iterations) iterations="$2"; shift 2;;
    --workload-us) workload_us="$2"; shift 2;;
    --deadline-us) deadline_us="$2"; shift 2;;
    --period-us) period_us="$2"; shift 2;;
    --seed) seed="$2"; shift 2;;
    --inject-period) inject_period="$2"; shift 2;;
    --inject-rate-pct) inject_rate_pct="$2"; shift 2;;
    --inject-delay-us) inject_delay_us="$2"; shift 2;;
    *) echo "unknown arg: $1"; exit 1;;
  esac
 done

timestamp=$(date +%Y%m%d_%H%M%S)
lf_file="${EVAL_DIR}/lf-gen/e2_microbench_N${N}.lf"
"${EVAL_DIR}/scripts/gen_microbench.py" \
  --out "$lf_file" \
  --reactions "$N" \
  --workload-us "$workload_us" \
  --deadline-us "$deadline_us" \
  --period-us "$period_us" \
  --steps "$iterations" \
  --seed "$seed" \
  --experiment "e2"

compile_lf "$lf_file"
name="$(basename "$lf_file" .lf)"

log_dir="${EVAL_DIR}/logs/e2/${timestamp}/intervene/N${N}_W${workload_us}"
export MS_MODE="intervene"
export LF_MS_DISABLE=0
export LF_MS_OBSERVE_ONLY=0
export MS_EVAL_INJECT_PERIOD="$inject_period"
export MS_EVAL_INJECT_RATE_PCT="$inject_rate_pct"
export MS_EVAL_INJECT_DELAY_US="$inject_delay_us"

run_program "$name" "$log_dir"

echo "E2 logs written under ${EVAL_DIR}/logs/e2/${timestamp}"
