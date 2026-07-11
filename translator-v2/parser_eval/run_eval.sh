#!/usr/bin/env bash
# Run the Flang vs fparser2 parser evaluation suite (WSL/Linux).
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." &>/dev/null && pwd)"
SUITE_DIR="${ROOT_DIR}/translator-v2/parser_eval"
SCAN_BIN="${ROOT_DIR}/translator-v2/flang-scan/build/op2-flang-scan"

export OP2_COMPILER="${OP2_COMPILER:-gnu}"
export CUDA_INSTALL_PATH="${CUDA_INSTALL_PATH:-/usr/local/cuda}"
export OP2_FLANG_SCAN="${SCAN_BIN}"

if [[ ! -x "${SCAN_BIN}" ]]; then
  echo "op2-flang-scan not found at ${SCAN_BIN}" >&2
  echo "Build with: cd translator-v2/flang-scan && cmake -B build -G Ninja && cmake --build build" >&2
  exit 1
fi

# Ensure OP2 libs exist for runtime builds.
make -C "${ROOT_DIR}/op2" -j"$(nproc)" >/dev/null

# Ensure translator venv.
make --silent -C "${ROOT_DIR}/translator-v2" python

exec "${ROOT_DIR}/translator-v2/.python/bin/python3" \
  "${SUITE_DIR}/eval_parsers.py" "$@"
