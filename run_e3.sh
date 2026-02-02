#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EVAL_DIR="${ROOT_DIR}/ms-eval"
source "${EVAL_DIR}/scripts/run_common.sh"

hc=8
lc=8
hc_work_us=80
lc_work_us=60
hc_deadline_us=300
lc_deadline_us=300
period_us=1000
steps=500
seed=1
load_factors="1.0,1.5,2.0,2.5"
lc_budget=4
window_ns=1000000

while [[ $# -gt 0 ]]; do
  case "$1" in
    --hc) hc="$2"; shift 2;;
    --lc) lc="$2"; shift 2;;
    --hc-work-us) hc_work_us="$2"; shift 2;;
    --lc-work-us) lc_work_us="$2"; shift 2;;
    --hc-deadline-us) hc_deadline_us="$2"; shift 2;;
    --lc-deadline-us) lc_deadline_us="$2"; shift 2;;
    --period-us) period_us="$2"; shift 2;;
    --steps) steps="$2"; shift 2;;
    --seed) seed="$2"; shift 2;;
    --load-factors) load_factors="$2"; shift 2;;
    --lc-budget) lc_budget="$2"; shift 2;;
    --window-ns) window_ns="$2"; shift 2;;
    *) echo "unknown arg: $1"; exit 1;;
  esac
 done

IFS=',' read -r -a LOAD_LIST <<< "$load_factors"

timestamp=$(date +%Y%m%d_%H%M%S)
lf_file="${EVAL_DIR}/lf-gen/e3_overload.lf"
"${EVAL_DIR}/scripts/gen_overload.py" \
  --out "$lf_file" \
  --hc "$hc" \
  --lc "$lc" \
  --hc-work-us "$hc_work_us" \
  --lc-work-us "$lc_work_us" \
  --hc-deadline-us "$hc_deadline_us" \
  --lc-deadline-us "$lc_deadline_us" \
  --period-us "$period_us" \
  --steps "$steps" \
  --seed "$seed" \
  --load-factor 1.0

compile_lf "$lf_file"
name="$(basename "$lf_file" .lf)"

for L in "${LOAD_LIST[@]}"; do
  for mode in baseline degrade; do
    log_dir="${EVAL_DIR}/logs/e3/${timestamp}/${mode}/L${L}"
    mkdir -p "$log_dir"
    cat > "${log_dir}/run_config.json" <<JSON
{
  "hc": ${hc},
  "lc": ${lc},
  "steps": ${steps},
  "mode": "${mode}",
  "load_factor": ${L}
}
JSON

    export MS_MODE="$mode"
    export MS_EVAL_LOAD_FACTOR="$L"

    if [[ "$mode" == "baseline" ]]; then
      export LF_MS_DISABLE=1
      export LF_MS_OBSERVE_ONLY=0
      unset LF_MS_CONFIG
    else
      export LF_MS_DISABLE=0
      export LF_MS_OBSERVE_ONLY=0
      cfg="${log_dir}/ms_policy.cfg"
      "${EVAL_DIR}/scripts/gen_ms_config.py" \
        --out "$cfg" \
        --hc "$hc" \
        --lc "$lc" \
        --lc-budget "$lc_budget" \
        --window-ns "$window_ns"
      export LF_MS_CONFIG="$cfg"
    fi

    run_program "$name" "$log_dir"
  done
 done

echo "E3 logs written under ${EVAL_DIR}/logs/e3/${timestamp}"
