#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
EVAL_DIR="${ROOT_DIR}/ms-eval"

fail() {
  echo "error: $*" >&2
  exit 1
}

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || fail "missing command: $1"
}

lfc_bin() {
  if [[ -n "${LFC:-}" ]]; then
    echo "$LFC"
  else
    echo "lfc"
  fi
}

compile_lf() {
  local lf_file="$1"
  local lfc
  lfc="$(lfc_bin)"
  need_cmd "$lfc"
  "$lfc" ${LFC_ARGS:-} "$lf_file"
}

find_exe() {
  local name="$1"
  local exe
  for exe in "${ROOT_DIR}/src-gen/ms-eval/lf-gen/${name}/build/${name}" \
             "${ROOT_DIR}/src-gen/ms-eval/lf-gen/${name}/${name}" \
             "${ROOT_DIR}/src-gen/${name}/${name}" \
             "${ROOT_DIR}/src-gen/${name}/build/${name}" \
             "${ROOT_DIR}/src-gen/${name}/bin/${name}" \
             "${ROOT_DIR}/bin/${name}" \
             "${ROOT_DIR}/bin/${name}.exe"; do
    if [[ -x "$exe" ]]; then
      echo "$exe"
      return 0
    fi
  done
  exe=$(find "${ROOT_DIR}" -maxdepth 4 -type f -name "${name}" -perm -111 2>/dev/null | head -n 1 || true)
  [[ -n "$exe" ]] && echo "$exe" && return 0
  return 1
}

run_program() {
  local name="$1"
  local log_dir="$2"
  local app_log="$log_dir/app.jsonl"
  local ms_log="$log_dir/ms.log"
  local run_prefix="${LF_RUN_PREFIX:-}"
  mkdir -p "$log_dir"

  local exe
  exe=$(find_exe "$name") || fail "could not find executable for ${name} (expected in bin/ or src-gen/)"

  if [[ -n "$run_prefix" ]]; then
    LF_APP_LOG="$app_log" \
    LF_MS_LOG="$ms_log" \
    LF_MS_LOG_LEVEL="${LF_MS_LOG_LEVEL:-INFO}" \
    bash -lc "${run_prefix} \"$exe\""
  else
    LF_APP_LOG="$app_log" \
    LF_MS_LOG="$ms_log" \
    LF_MS_LOG_LEVEL="${LF_MS_LOG_LEVEL:-INFO}" \
    "$exe"
  fi
}
