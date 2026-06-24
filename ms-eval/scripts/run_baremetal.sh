#!/usr/bin/env bash
# Bare-metal (no Docker) runner for the TCRS evaluation, intended for a dedicated
# Linux host such as a Raspberry Pi 4/5 (quad-core aarch64). Lives in
# reactor-c/ms-eval/scripts and runs the orchestrator that sits next to it.
#
#   ./run_baremetal.sh <mode> [cpuset]
#
# <mode>   one of the run_e4_e6_main.py modes: backlog | validate | periods |
#          final | diag | dry  (see experiments/RUN_BAREMETAL_RPI.md)
# cpuset   cores to pin the run to (default: 0-3; use isolated cores, e.g. 2-3)
#
# Repo root and data dir are auto-detected from this script's location; override
# with TCRS_REACTOR / TCRS_DATA. Results go to <repo>/ms-eval/tcrs-data by default.
set -euo pipefail

MODE="${1:-dry}"
CPUSET="${2:-0-3}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="${TCRS_REACTOR:-$(cd "$SCRIPT_DIR/../.." && pwd)}"
export TCRS_REACTOR="$REPO"

# --- preflight -----------------------------------------------------------
command -v python3 >/dev/null || { echo "ERROR: python3 not found" >&2; exit 1; }
command -v cmake   >/dev/null || { echo "ERROR: cmake not found (apt install cmake)" >&2; exit 1; }
command -v taskset >/dev/null || { echo "ERROR: taskset not found (apt install util-linux)" >&2; exit 1; }
[ -x "$REPO/run_e3.sh" ] || { echo "ERROR: $REPO/run_e3.sh missing/not executable" >&2; exit 1; }
if ! command -v lfc >/dev/null && [ -z "${LFC_BIN:-}" ]; then
  echo "WARNING: 'lfc' not on PATH. Set LFC_BIN or add the Lingua Franca bin/ to PATH." >&2
fi
[ -n "${LFC_BIN:-}" ] && export PATH="$(dirname "$LFC_BIN"):$PATH"

# --- best-effort timing hygiene (needs privileges; warn, do not fail) ----
if command -v cpupower >/dev/null 2>&1; then
  sudo cpupower frequency-set -g performance >/dev/null 2>&1 \
    && echo "cpufreq governor: performance" \
    || echo "NOTE: could not set performance governor (run with sudo / not supported)."
fi
echo "Reminder: ensure active cooling + an adequate PSU so the Pi does not"
echo "thermally throttle (throttling injects jitter). Check: vcgencmd get_throttled"

# --- run -----------------------------------------------------------------
echo "REPO=$REPO"
echo "DATA=${TCRS_DATA:-$REPO/ms-eval/tcrs-data}"
echo "Pinning run to cores: $CPUSET   mode: $MODE"
exec taskset -c "$CPUSET" python3 "$SCRIPT_DIR/run_e4_e6_main.py" --mode "$MODE"
