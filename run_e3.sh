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
workers=4
degrade_lag_ns=200000
degrade_ready_q_len=2
stress_cpu=0
stress_load=85
stress_timeout_s=60
stress_warmup_s=2
build_jobs="${MS_EVAL_BUILD_JOBS:-1}"
skip_compile=0
lf_cpu_set=""
stress_cpu_set=""

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
    --workers) workers="$2"; shift 2;;
    --degrade-lag-ns) degrade_lag_ns="$2"; shift 2;;
    --degrade-ready-q-len) degrade_ready_q_len="$2"; shift 2;;
    --stress-cpu) stress_cpu="$2"; shift 2;;
    --stress-load) stress_load="$2"; shift 2;;
    --stress-timeout-s) stress_timeout_s="$2"; shift 2;;
    --stress-warmup-s) stress_warmup_s="$2"; shift 2;;
    --skip-compile) skip_compile="$2"; shift 2;;
    --lf-cpu-set) lf_cpu_set="$2"; shift 2;;
    --stress-cpu-set) stress_cpu_set="$2"; shift 2;;
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
  --load-factor 1.0 \
  --workers "$workers"

name="$(basename "$lf_file" .lf)"
exe=""
ms_patch_dir="$(mktemp -d "${ROOT_DIR}/.e3_runtime_patch.XXXXXX")"
cp "${ROOT_DIR}/core/utils/master_scheduler.c" "${ms_patch_dir}/master_scheduler.c"
cp "${ROOT_DIR}/include/core/utils/master_scheduler.h" "${ms_patch_dir}/master_scheduler.h"
cp "${ROOT_DIR}/core/threaded/reactor_threaded.c" "${ms_patch_dir}/reactor_threaded.c"
cp "${ROOT_DIR}/core/threaded/scheduler_NP.c" "${ms_patch_dir}/scheduler_NP.c"
cp "${ROOT_DIR}/core/threaded/scheduler_adaptive.c" "${ms_patch_dir}/scheduler_adaptive.c"
cp "${ROOT_DIR}/core/threaded/scheduler_GEDF_NP.c" "${ms_patch_dir}/scheduler_GEDF_NP.c"
cp "${ROOT_DIR}/core/utils/CMakeLists.txt" "${ms_patch_dir}/core_utils_CMakeLists.txt"
trap 'rm -rf "${ms_patch_dir}"' EXIT

if [[ "$skip_compile" -eq 0 ]]; then
  compile_lf "$lf_file"
  exe="$(find_exe "$name")"
else
  exe="$(find_exe "$name")" || fail "could not find precompiled executable for ${name}; run once without --skip-compile"
fi

# lfc may regenerate runtime sources under src-gen; force our local MS runtime
# implementation into that tree and rebuild so E3 uses the patched scheduler.
gen_dir="${ROOT_DIR}/src-gen/ms-eval/lf-gen/${name}"
if [[ "$skip_compile" -eq 0 && -d "${gen_dir}/core/utils" && -d "${gen_dir}/include/core/utils" && -d "${gen_dir}/core/threaded" ]]; then
  mkdir -p "${ROOT_DIR}/include/core/utils"
  cp "${ms_patch_dir}/master_scheduler.h" "${ROOT_DIR}/include/core/utils/master_scheduler.h"
  cp "${ms_patch_dir}/core_utils_CMakeLists.txt" "${gen_dir}/core/utils/CMakeLists.txt"
  cp "${ms_patch_dir}/master_scheduler.c" "${gen_dir}/core/utils/master_scheduler.c"
  cp "${ms_patch_dir}/master_scheduler.h" "${gen_dir}/include/core/utils/master_scheduler.h"
  cp "${ms_patch_dir}/reactor_threaded.c" "${gen_dir}/core/threaded/reactor_threaded.c"
  cp "${ms_patch_dir}/scheduler_NP.c" "${gen_dir}/core/threaded/scheduler_NP.c"
  cp "${ms_patch_dir}/scheduler_adaptive.c" "${gen_dir}/core/threaded/scheduler_adaptive.c"
  cp "${ms_patch_dir}/scheduler_GEDF_NP.c" "${gen_dir}/core/threaded/scheduler_GEDF_NP.c"
  cmake --build "${gen_dir}/build" --target install --parallel "${build_jobs}"
  exe="$(find_exe "$name")"
fi

# Probe runtime reaction indices (which may not be 0..N-1 on this runtime build).
probe_dir="${EVAL_DIR}/logs/e3/${timestamp}/probe"
mkdir -p "$probe_dir"
probe_ms_log="${probe_dir}/ms.log"
probe_app_log="${probe_dir}/app.jsonl"
rm -f "$probe_ms_log" "$probe_app_log"
set +e
timeout 5s env \
  LF_APP_LOG="$probe_app_log" \
  LF_MS_LOG="$probe_ms_log" \
  LF_MS_LOG_LEVEL=DEBUG \
  LF_MS_MINIMAL_LOG=0 \
  MS_EVAL_LOG_REACTION_START=1 \
  LF_MS_DISABLE=0 \
  LF_MS_OBSERVE_ONLY=1 \
  "$exe" >/dev/null 2>&1
set -e

REACTION_INDICES="$(
  python3 - "$probe_ms_log" "$probe_app_log" "$((hc + lc))" <<'PY'
import json
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

ms_path = Path(sys.argv[1])
app_path = Path(sys.argv[2])
total = int(sys.argv[3])

if not ms_path.exists() or not app_path.exists():
    sys.exit(0)

selected = []
for line in ms_path.read_text(errors="ignore").splitlines():
    m = re.search(r"event=runtime_selected env=0 reaction_index=([0-9]+) physical=([0-9]+)", line)
    if m:
        selected.append((int(m.group(2)), int(m.group(1))))

starts = []
for line in app_path.read_text(errors="ignore").splitlines():
    try:
        obj = json.loads(line)
    except json.JSONDecodeError:
        continue
    if obj.get("type") == "reaction_start":
        starts.append((int(obj["ts_mono_ns"]), int(obj["reaction_id"])))

if not selected or not starts:
    sys.exit(0)

# Map runtime reaction_index -> generator reaction_id using nearest timestamp.
mapping_votes = defaultdict(Counter)
j = 0
for ts, ridx in selected:
    while j + 1 < len(starts) and starts[j + 1][0] <= ts:
        j += 1
    candidates = []
    for k in (j - 1, j, j + 1, j + 2):
        if 0 <= k < len(starts):
            d = abs(starts[k][0] - ts)
            candidates.append((d, k))
    if not candidates:
        continue
    d, k = min(candidates)
    if d <= 500_000:  # 0.5ms tolerance
        mapping_votes[ridx][starts[k][1]] += 1

# Invert to reaction_id -> most voted runtime index.
rid_to_idx = {}
for ridx, ctr in mapping_votes.items():
    rid, _ = ctr.most_common(1)[0]
    # Prefer the runtime index with more votes if duplicated.
    if rid not in rid_to_idx or ctr[rid] > mapping_votes[rid_to_idx[rid]][rid]:
        rid_to_idx[rid] = ridx

ordered = []
for rid in range(total):
    idx = rid_to_idx.get(rid)
    if idx is None:
        break
    ordered.append(str(idx))

if len(ordered) == total:
    print(",".join(ordered))
PY
)"
if [[ -z "${REACTION_INDICES}" ]]; then
  echo "warning: failed to derive rid->runtime index mapping; falling back to 0..N-1 mapping" >&2
else
  echo "derived rid-aligned reaction indices: ${REACTION_INDICES}" >&2
fi

EXTRA_HIGH_INDICES="$(
  python3 - "$probe_ms_log" "${REACTION_INDICES}" <<'PY'
import re
import sys
from pathlib import Path

ms_path = Path(sys.argv[1])
mapped = {int(x) for x in sys.argv[2].split(",") if x.strip()}
if not ms_path.exists():
    sys.exit(0)

seen = []
for line in ms_path.read_text(errors="ignore").splitlines():
    m = re.search(r"event=runtime_selected env=0 reaction_index=([0-9]+)", line)
    if not m:
        continue
    ridx = int(m.group(1))
    if ridx not in seen:
        seen.append(ridx)

extras = [str(x) for x in seen if x not in mapped]
print(",".join(extras))
PY
)"

for L in "${LOAD_LIST[@]}"; do
  for mode in baseline ms degrade; do
    log_dir="${EVAL_DIR}/logs/e3/${timestamp}/${mode}/L${L}"
    mkdir -p "$log_dir"
    cat > "${log_dir}/run_config.json" <<JSON
{
  "hc": ${hc},
  "lc": ${lc},
  "workers": ${workers},
  "steps": ${steps},
  "mode": "${mode}",
  "load_factor": ${L},
  "degrade_lag_ns": ${degrade_lag_ns},
  "degrade_ready_q_len": ${degrade_ready_q_len}
}
JSON

    export MS_MODE="$mode"
    export MS_EVAL_LOAD_FACTOR="$L"
    export MS_EVAL_LOG_REACTION_START="${MS_EVAL_LOG_REACTION_START:-0}"
    export LF_MS_MINIMAL_LOG="${LF_MS_MINIMAL_LOG:-1}"

    if [[ "$mode" == "baseline" ]]; then
      export LF_MS_DISABLE=1
      export LF_MS_OBSERVE_ONLY=0
      export LF_MS_DEGRADE_ENABLE=0
      unset LF_MS_CONFIG
    elif [[ "$mode" == "ms" ]]; then
      export LF_MS_DISABLE=0
      export LF_MS_OBSERVE_ONLY=0
      export LF_MS_DEGRADE_ENABLE=0
      unset LF_MS_CONFIG
    else
      export LF_MS_DISABLE=0
      export LF_MS_OBSERVE_ONLY=0
      export LF_MS_DEGRADE_ENABLE=1
      export LF_MS_DEGRADE_LAG_NS="${degrade_lag_ns}"
      export LF_MS_DEGRADE_READY_Q_LEN="${degrade_ready_q_len}"
      cfg="${log_dir}/ms_policy.cfg"
      "${EVAL_DIR}/scripts/gen_ms_config.py" \
        --out "$cfg" \
        --hc "$hc" \
        --lc "$lc" \
        --lc-budget "$lc_budget" \
        --window-ns "$window_ns" \
        --reaction-indices "${REACTION_INDICES}" \
        --extra-high-indices "${EXTRA_HIGH_INDICES}"
      export LF_MS_CONFIG="$cfg"
    fi

    stress_pid=""
    if [[ "$stress_cpu" -gt 0 ]]; then
      need_cmd stress-ng
      if [[ -n "$stress_cpu_set" ]]; then
        taskset -c "$stress_cpu_set" \
          stress-ng --cpu "$stress_cpu" --cpu-load "$stress_load" --timeout "${stress_timeout_s}s" \
          >/tmp/stress_ng_e3_${timestamp}_${mode}_L${L}.log 2>&1 &
      else
        stress-ng --cpu "$stress_cpu" --cpu-load "$stress_load" --timeout "${stress_timeout_s}s" \
          >/tmp/stress_ng_e3_${timestamp}_${mode}_L${L}.log 2>&1 &
      fi
      stress_pid=$!
      if [[ "$stress_warmup_s" -gt 0 ]]; then
        sleep "$stress_warmup_s"
      fi
    fi

    if [[ -n "$lf_cpu_set" ]]; then
      export LF_RUN_PREFIX="taskset -c ${lf_cpu_set}"
    else
      unset LF_RUN_PREFIX
    fi
    run_program "$name" "$log_dir"
    unset LF_RUN_PREFIX

    if [[ -n "$stress_pid" ]]; then
      kill "$stress_pid" >/dev/null 2>&1 || true
      wait "$stress_pid" >/dev/null 2>&1 || true
    fi
  done
 done

echo "E3 logs written under ${EVAL_DIR}/logs/e3/${timestamp}"
