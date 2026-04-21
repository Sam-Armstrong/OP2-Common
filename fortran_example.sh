#!/usr/bin/env bash
# Builds and runs the Fortran airfoil example using the OP2 code generators.

set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
APP_DIR="${ROOT_DIR}/apps/fortran/airfoil"
GRID_URL="https://op-dsl.github.io/docs/OP2/new_grid.dat"

# Use one toolchain for C/C++ and Fortran so "make config" loads both
# compilers/c/gnu.mk and compilers/fortran/gnu.mk (OP2_F_COMPILER alone skips C).
# See https://op2-dsl.readthedocs.io/en/latest/getting_started.html
export OP2_COMPILER="gnu"
export CUDA_INSTALL_PATH="/usr/local/cuda"

for cmd in gcc g++ gfortran; do
    if ! command -v "${cmd}" >/dev/null 2>&1; then
        echo "This example needs ${cmd} on PATH (e.g. build-essential and gfortran on Debian/Ubuntu)." >&2
        exit 1
    fi
done

# Regenerate config only when needed. Always running "make config" in a minimal
# environment (no gcc on PATH) would overwrite a good toolchain; skipping config
# when gfortran was absent during an earlier "make config" leaves HAVE_F unset
# and op2/mod empty.
CFG_MK="${ROOT_DIR}/makefiles/.config.mk"
need_config=false
if [ ! -f "${CFG_MK}" ]; then
    need_config=true
elif ! grep -q "HAVE_C := true" "${CFG_MK}"; then
    need_config=true
elif ! grep -q "HAVE_F := true" "${CFG_MK}"; then
    need_config=true
fi
if [ "${need_config}" = true ]; then
    make -C "${ROOT_DIR}/op2" config
fi

make -C "${ROOT_DIR}/op2" -j"$(nproc)"

# WSL DrvFS (/mnt/...) often breaks Fortran module writes and Python codegen mkdir.
# Put generated sources and .mod output on a Linux-native filesystem (see makefiles/f_app.mk).
if [[ "${ROOT_DIR}" == /mnt/* ]]; then
    tag="$(printf "%s" "${ROOT_DIR}" | sha256sum | cut -c1-16)"
    _app_out="${TMPDIR:-/tmp}/op2_fortran_airfoil_${tag}"
    export F_APP_GENERATED_DIR="${_app_out}/generated"
    export F_APP_MOD_DIR="${_app_out}/mod"
    mkdir -p "${F_APP_GENERATED_DIR}" "${F_APP_MOD_DIR}"
    unset _app_out
fi

# Build only the variants whose Fortran codegen schemes are registered:
#   - seq:    developer sequential (no codegen)
#   - genseq: sequential from the OP2 code generator
# Use -j1: parallel gfortran to the same -J dir can fail on some filesystems (e.g. /mnt/c).
make -C "${APP_DIR}" -j1 airfoil_plain_seq airfoil_plain_genseq

if [ ! -f "${APP_DIR}/new_grid.dat" ]; then
    echo "Downloading mesh from ${GRID_URL}"
    curl -L -o "${APP_DIR}/new_grid.dat" "${GRID_URL}"
fi

cd "${APP_DIR}"
./airfoil_plain_genseq
