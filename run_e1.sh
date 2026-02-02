#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EVAL_DIR="${ROOT_DIR}/ms-eval"
source "${EVAL_DIR}/scripts/run_common.sh"

Ns="16,32,64,128"
iterations=1000
workload_us=50
deadline_us=200
period_us=1000
seed=1

while [[ $# -gt 0 ]]; do
  case "$1" in
    --N) Ns="$2"; shift 2;;
    --iterations) iterations="$2"; shift 2;;
    --workload-us) workload_us="$2"; shift 2;;
    --deadline-us) deadline_us="$2"; shift 2;;
    --period-us) period_us="$2"; shift 2;;
    --seed) seed="$2"; shift 2;;
    *) echo "unknown arg: $1"; exit 1;;
  esac
 done

IFS=',' read -r -a N_LIST <<< "$Ns"

timestamp=$(date +%Y%m%d_%H%M%S)

for N in "${N_LIST[@]}"; do
  lf_file="${EVAL_DIR}/lf-gen/e1_microbench_N${N}.lf"
  "${EVAL_DIR}/scripts/gen_microbench.py" \
    --out "$lf_file" \
    --reactions "$N" \
    --workload-us "$workload_us" \
    --deadline-us "$deadline_us" \
    --period-us "$period_us" \
    --steps "$iterations" \
    --seed "$seed" \
    --experiment "e1"

  compile_lf "$lf_file"
  name="$(basename "$lf_file" .lf)"

  for mode in baseline observe intervene; do
    log_dir="${EVAL_DIR}/logs/e1/${timestamp}/${mode}/N${N}_W${workload_us}"
    export MS_MODE="$mode"
    if [[ "$mode" == "baseline" ]]; then
      export LF_MS_DISABLE=1
      export LF_MS_OBSERVE_ONLY=0
    elif [[ "$mode" == "observe" ]]; then
      export LF_MS_DISABLE=0
      export LF_MS_OBSERVE_ONLY=1
    else
      export LF_MS_DISABLE=0
      export LF_MS_OBSERVE_ONLY=0
    fi

    run_program "$name" "$log_dir"
  done
 done

echo "E1 logs written under ${EVAL_DIR}/logs/e1/${timestamp}"
