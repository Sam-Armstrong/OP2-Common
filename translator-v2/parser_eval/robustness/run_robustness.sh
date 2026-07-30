#!/usr/bin/env bash
# Run the fparser2-failing OP2 robustness suite (WSL/Linux).
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../../.." &>/dev/null && pwd)"
SUITE_DIR="${ROOT_DIR}/translator-v2/parser_eval/robustness"
SCAN_BIN="${ROOT_DIR}/translator-v2/flang-scan/build/op2-flang-scan"

export OP2_FLANG_SCAN="${SCAN_BIN}"

if [[ ! -x "${SCAN_BIN}" ]]; then
  echo "op2-flang-scan not found at ${SCAN_BIN}" >&2
  echo "Build with: cd translator-v2/flang-scan && cmake -B build -G Ninja && cmake --build build" >&2
  exit 1
fi

make --silent -C "${ROOT_DIR}/translator-v2" python

exec "${ROOT_DIR}/translator-v2/.python/bin/python3" \
  "${SUITE_DIR}/eval_robustness.py" "$@"
