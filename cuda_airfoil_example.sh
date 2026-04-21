#!/usr/bin/env bash
# Builds and runs the C airfoil example on CUDA (OP2 CUDA backend).

set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
APP_DIR="${ROOT_DIR}/apps/c/airfoil/airfoil_plain/dp"
GRID_URL="https://op-dsl.github.io/docs/OP2/new_grid.dat"

# Supported toolchains: gnu, cray, intel, xl, nvhpc (nvhpc often used with CUDA)
export OP2_COMPILER="${OP2_COMPILER:-gnu}"

# optional: set if CUDA is not on the default path (WSL example)
export CUDA_INSTALL_PATH="/usr/local/cuda"

# nvcc needs explicit -gencode: default SM must match your GPU (e.g. RTX 3080 = sm_86).
# If unset, use the first GPU's compute capability from nvidia-smi, else Ampere sm_80+86.
if [ -z "${CUDA_GEN:-}" ] && [ -z "${NV_ARCH:-}" ]; then
    if command -v nvidia-smi >/dev/null 2>&1; then
        cap="$(nvidia-smi --query-gpu=compute_cap --format=csv,noheader 2>/dev/null | head -n1 | tr -d '[:space:]')"
        if [ -n "${cap}" ]; then
            major="${cap%%.*}"
            minor="${cap##*.}"
            export CUDA_GEN="$((major * 10 + minor))"
        fi
    fi
    export CUDA_GEN="${CUDA_GEN:-80,86}"
fi

CONFIG_MK="${ROOT_DIR}/makefiles/.config.mk"

# make writes .config.mk lines with leading spaces; match that or column 0.
cuda_recorded_in_config() {
    [ -f "${CONFIG_MK}" ] || return 1
    grep -qE '^[[:space:]]*HAVE_C_CUDA := true' "${CONFIG_MK}" \
        && grep -qE '^[[:space:]]*HAVE_CUDA := true' "${CONFIG_MK}"
}

# Stale config often has NVCCFLAGS without -gencode (nvcc then picks an SM that breaks the build).
nvcc_targets_gpu_arch() {
    grep -qE -- '-gencode|arch=compute_' "${CONFIG_MK}" 2>/dev/null
}

# Reconfigure if the primary GPU's SM is not in NVCCFLAGS (e.g. config had sm_60 but GPU is sm_86).
nvcc_config_matches_gpu() {
    [ -f "${CONFIG_MK}" ] || return 1
    command -v nvidia-smi >/dev/null 2>&1 || return 0
    cap="$(nvidia-smi --query-gpu=compute_cap --format=csv,noheader 2>/dev/null | head -n1 | tr -d '[:space:]')"
    [ -n "${cap}" ] || return 0
    major="${cap%%.*}"
    minor="${cap##*.}"
    sm="$((major * 10 + minor))"
    grep -qF "code=sm_${sm}" "${CONFIG_MK}" 2>/dev/null
}

refresh_op2_config=false
if [ ! -f "${CONFIG_MK}" ] || ! cuda_recorded_in_config \
    || ! nvcc_targets_gpu_arch || ! nvcc_config_matches_gpu; then
    make -C "${ROOT_DIR}/op2" config
    refresh_op2_config=true
fi

if ! cuda_recorded_in_config; then
    echo "CUDA toolkit or CUDA libraries were not detected when config was generated." >&2
    echo "In WSL: install nvidia-cuda-toolkit or NVIDIA's Linux CUDA, set CUDA_INSTALL_PATH if needed, then:" >&2
    echo "  make -C \"${ROOT_DIR}/op2\" clean_config config" >&2
    exit 1
fi

make -C "${ROOT_DIR}/op2" -j"$(nproc)"

# NVCCFLAGS live in .config.mk; make does not always rebuild *.cu when only those change.
if [ "${refresh_op2_config}" = true ]; then
    rm -f "${APP_DIR}/generated/airfoil/cuda/op2_kernels.o" "${APP_DIR}/airfoil_cuda"
fi

make -C "${APP_DIR}" -j"$(nproc)" airfoil_cuda

if [ ! -f "${APP_DIR}/new_grid.dat" ]; then
    echo "Downloading mesh from ${GRID_URL}"
    curl -L -o "${APP_DIR}/new_grid.dat" "${GRID_URL}"
fi

cd "${APP_DIR}"
./airfoil_cuda OP_PART_SIZE=128 OP_BLOCK_SIZE=192
