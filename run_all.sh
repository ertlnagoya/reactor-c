#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EVAL_DIR="${ROOT_DIR}/ms-eval"

"${ROOT_DIR}/run_e1.sh" "$@"
"${ROOT_DIR}/run_e2.sh"
"${ROOT_DIR}/run_e3.sh"

python3 "${EVAL_DIR}/scripts/collect_metadata.py"
python3 "${EVAL_DIR}/scripts/parse_e1.py"
python3 "${EVAL_DIR}/scripts/parse_e2.py"
python3 "${EVAL_DIR}/scripts/parse_e3.py"
python3 "${EVAL_DIR}/scripts/plot_fig2.py"
python3 "${EVAL_DIR}/scripts/plot_fig3.py"

echo "Results written to ${EVAL_DIR}/results and ${EVAL_DIR}/figures"
