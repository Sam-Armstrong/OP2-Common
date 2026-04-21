#!/usr/bin/env bash
# Builds and runs the Fortran airfoil example using the OP2 code generators.

set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
APP_DIR="${ROOT_DIR}/apps/fortran/airfoil"
GRID_URL="https://op-dsl.github.io/docs/OP2/new_grid.dat"

# Supported toolchains: gnu, cray, intel, xl, nvhpc
export OP2_COMPILER="${OP2_COMPILER:-gnu}"

# Build the OP2 runtime libraries
if [ ! -f "${ROOT_DIR}/makefiles/.config.mk" ]; then
    make -C "${ROOT_DIR}/op2" config
fi
make -C "${ROOT_DIR}/op2" -j"$(nproc)"

# Build only the variants whose Fortran codegen schemes are registered:
#   - seq:    developer sequential (no codegen)
#   - genseq: sequential from the OP2 code generator
make -C "${APP_DIR}" -j"$(nproc)" airfoil_plain_seq airfoil_plain_genseq

if [ ! -f "${APP_DIR}/new_grid.dat" ]; then
    echo "Downloading mesh from ${GRID_URL}"
    curl -L -o "${APP_DIR}/new_grid.dat" "${GRID_URL}"
fi

cd "${APP_DIR}"
./airfoil_plain_genseq
