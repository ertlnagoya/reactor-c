#!/usr/bin/env bash
# TCRS 2026 experiments (E4-E6) inside the lf-ms-phase4 container. Lives in
# reactor-c/ms-eval/scripts; the repo root is auto-detected. Prefer the native
# runner (run_baremetal.sh) on real hardware; this is the Docker convenience path.
#
#   ./run_e4_e6_docker.sh <mode>      # backlog | validate | periods | final | ...
#
# The repo is mounted at its host path so absolute paths resolve unchanged, then
# scripts/run_e4_e6_main.py runs inside the container. Results land in
# <repo>/ms-eval/tcrs-data/.
set -euo pipefail

MODE="${1:-dry}"
IMAGE="${LF_MS_IMAGE:-lf-ms-phase4:latest}"
CPUSET="${LF_MS_CPUSET:-0-3}"     # pin for timing stability; set empty to disable
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="${TCRS_REACTOR:-$(cd "$SCRIPT_DIR/../.." && pwd)}"

case "$MODE" in
  calibrate|verify|validate|backlog|diag|periods|final|smoke|full|dry)
    ORCH_ARGS="--mode $MODE" ;;
  *) echo "usage: $0 [calibrate|verify|validate|backlog|diag|periods|final|full|smoke|dry]" >&2; exit 2 ;;
esac

command -v docker >/dev/null 2>&1 || { echo "ERROR: docker not found." >&2; exit 1; }
docker info >/dev/null 2>&1 || { echo "ERROR: Docker daemon not reachable." >&2; exit 1; }
docker image inspect "$IMAGE" >/dev/null 2>&1 || {
  echo "ERROR: image '$IMAGE' not found. Build from the repo root: docker build --no-cache -t lf-ms-phase4 ." >&2
  exit 1; }

CPUSET_OPT=""; [ -n "$CPUSET" ] && CPUSET_OPT="--cpuset-cpus $CPUSET"
echo "Mode=$MODE  Image=$IMAGE  Repo=$REPO  Cpuset=${CPUSET:-none}"

# --cap-add=SYS_NICE + rtprio ulimit let the OS-RT (SCHED_FIFO) validation arm
# raise HC worker priority; harmless for the other modes.
# shellcheck disable=SC2086
docker run --rm -t $CPUSET_OPT \
  --cap-add=SYS_NICE --ulimit rtprio=99 \
  -v "$REPO":"$REPO" \
  -w "$REPO" \
  "$IMAGE" \
  bash -lc "set -e; TCRS_REACTOR='$REPO' python3 '$SCRIPT_DIR/run_e4_e6_main.py' $ORCH_ARGS"

echo
echo "Done. Results in $REPO/ms-eval/tcrs-data/{e4,e5,e6}/"
