#!/usr/bin/env bash
# Compare the wall-clock cost of the OP2 Fortran translator's Stage-1 parser
# between the default fparser2 backend and the LLVM Flang backend.
#
# Usage:
#   translator-v2/benchmark_parsers.sh [--runs N] [--app airfoil]
#                                      [--keep] [-- <files...>]
#
# Options:
#   --runs N          Number of measured runs per parser (default: 5).
#                     A single warmup run per parser is performed first and
#                     discarded from the summary.
#   --app NAME        Named app under apps/fortran/<NAME>/ (default: airfoil).
#   --keep            Do not delete the temp output directory on exit.
#   --                Everything after -- is the explicit source file list,
#                     overriding --app.
#
# Environment:
#   OP2_FLANG_SCAN    Path to the op2-flang-scan binary. If unset, defaults
#                     to translator-v2/flang-scan/build/op2-flang-scan.

set -euo pipefail

# ---------------------------------------------------------------------------
# Paths & defaults
# ---------------------------------------------------------------------------

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
ROOT_DIR="$(cd -- "${SCRIPT_DIR}/.." &>/dev/null && pwd)"
TRANSLATOR="${ROOT_DIR}/translator-v2/op2-translator.sh"
DEFAULT_FLANG_SCAN="${ROOT_DIR}/translator-v2/flang-scan/build/op2-flang-scan"

RUNS=5
APP_NAME="airfoil"
KEEP_TMP=0
declare -a EXPLICIT_FILES=()

while (($# > 0)); do
    case "$1" in
        --runs)  RUNS="$2"; shift 2 ;;
        --app)   APP_NAME="$2"; shift 2 ;;
        --keep)  KEEP_TMP=1; shift ;;
        --)      shift; EXPLICIT_FILES=("$@"); break ;;
        -h|--help) sed -n '2,22p' "$0"; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done

# ---------------------------------------------------------------------------
# Resolve inputs
# ---------------------------------------------------------------------------

APP_DIR="${ROOT_DIR}/apps/fortran/${APP_NAME}"

declare -a SOURCES=()
declare -a EXTRA_FLAGS=()

if ((${#EXPLICIT_FILES[@]} > 0)); then
    SOURCES=("${EXPLICIT_FILES[@]}")
else
    case "${APP_NAME}" in
        airfoil)
            SOURCES=(
                "${APP_DIR}/airfoil_constants.F90"
                "${APP_DIR}/airfoil_kernels.F90"
                "${APP_DIR}/airfoil.F90"
            )
            EXTRA_FLAGS=( -DPLAIN --consts-module airfoil_constants.F90 )
            ;;
        *)
            echo "No default source set for app '${APP_NAME}'." >&2
            echo "Pass files explicitly after --." >&2
            exit 2 ;;
    esac
fi

for f in "${SOURCES[@]}"; do
    [[ -f "$f" ]] || { echo "Missing input file: $f" >&2; exit 2; }
done

# ---------------------------------------------------------------------------
# Sanity checks
# ---------------------------------------------------------------------------

: "${OP2_FLANG_SCAN:=${DEFAULT_FLANG_SCAN}}"
if [[ ! -x "${OP2_FLANG_SCAN}" ]]; then
    echo "op2-flang-scan binary not found at: ${OP2_FLANG_SCAN}" >&2
    echo "Build it with:" >&2
    echo "    cd translator-v2/flang-scan && cmake --build build" >&2
    echo "or set OP2_FLANG_SCAN to a different path." >&2
    exit 2
fi
export OP2_FLANG_SCAN

# Run from the app directory so relative paths like --consts-module resolve
# identically to how apps/fortran/<APP>/Makefile invokes the translator.
cd "${APP_DIR}"

# ---------------------------------------------------------------------------
# Runner
# ---------------------------------------------------------------------------

TMP_ROOT="$(mktemp -d -t op2-parser-bench.XXXXXX)"
cleanup() {
    if ((KEEP_TMP == 0)); then
        rm -rf "${TMP_ROOT}"
    fi
}
trap cleanup EXIT

# Invoke the translator once, silencing its output, and echo elapsed seconds.
run_one() {
    local parser="$1"
    local out="${TMP_ROOT}/${parser}_$2"
    mkdir -p "${out}"

    local start_ns end_ns
    start_ns="$(date +%s%N)"

    local extra_args=()
    if ((${#EXTRA_FLAGS[@]} > 0)); then
        extra_args=("${EXTRA_FLAGS[@]}")
    fi

    if ! "${TRANSLATOR}" \
            ${extra_args[@]+"${extra_args[@]}"} \
            --parser "${parser}" \
            "${SOURCES[@]}" \
            -o "${out}" \
            >"${out}/stdout.log" 2>"${out}/stderr.log"; then
        echo "--- stderr (${parser}) ---" >&2
        cat "${out}/stderr.log" >&2 || true
        return 1
    fi

    end_ns="$(date +%s%N)"
    awk -v s="${start_ns}" -v e="${end_ns}" 'BEGIN { printf "%.4f\n", (e - s) / 1e9 }'
}

# Compute + print mean/median/min/max/stdev for a list of floats.
stats_row() {
    local label="$1"; shift
    printf '%s\n' "$@" | awk -v label="${label}" '
        {
            v = $1 + 0
            sum += v; sumsq += v * v
            if (n == 0 || v < min) min = v
            if (n == 0 || v > max) max = v
            xs[n++] = v
        }
        END {
            if (n == 0) { printf "%-12s  no samples\n", label; exit }
            mean = sum / n
            var  = (n > 1) ? (sumsq - n * mean * mean) / (n - 1) : 0
            if (var < 0) var = 0
            stdev = sqrt(var)
            asort(xs)
            if (n % 2) median = xs[int(n/2) + 1]
            else       median = (xs[n/2] + xs[n/2 + 1]) / 2
            printf "%-12s  n=%d  mean=%.3fs  median=%.3fs  min=%.3fs  max=%.3fs  stdev=%.3fs\n",
                   label, n, mean, median, min, max, stdev
        }'
}

mean_of() {
    printf '%s\n' "$@" | awk '{ s += $1; n++ } END { if (n) printf "%.6f", s/n }'
}

# ---------------------------------------------------------------------------
# Banner
# ---------------------------------------------------------------------------

echo "========================================================================"
echo "OP2 translator parser benchmark"
echo "  app         : ${APP_NAME}"
echo "  runs        : ${RUNS} measured (+1 warmup discarded)"
echo "  sources     :"
for f in "${SOURCES[@]}"; do echo "                ${f}"; done
echo "  translator  : ${TRANSLATOR}"
echo "  flang-scan  : ${OP2_FLANG_SCAN}"
echo "  tmp output  : ${TMP_ROOT}"
if ((KEEP_TMP == 1)); then
    echo "                (will be preserved on exit)"
fi
echo "========================================================================"
echo

# ---------------------------------------------------------------------------
# Execute: warmup then N runs per parser
# ---------------------------------------------------------------------------

declare -a FPARSER_TIMES=()
declare -a FLANG_TIMES=()

for parser in fparser2 flang; do
    echo "Warming up ${parser}..."
    run_one "${parser}" "warmup" >/dev/null
done
echo

for parser in fparser2 flang; do
    echo "Running ${parser} (${RUNS} iterations)"
    for ((i = 1; i <= RUNS; i++)); do
        t="$(run_one "${parser}" "r${i}")"
        printf "  run %2d: %8.3fs\n" "$i" "$t"
        case "${parser}" in
            fparser2) FPARSER_TIMES+=("$t") ;;
            flang)    FLANG_TIMES+=("$t") ;;
        esac
    done
    echo
done

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

echo "------------------------------------------------------------------------"
echo "Summary"
echo "------------------------------------------------------------------------"
stats_row "fparser2" "${FPARSER_TIMES[@]}"
stats_row "flang"    "${FLANG_TIMES[@]}"

mean_fp="$(mean_of "${FPARSER_TIMES[@]}")"
mean_fl="$(mean_of "${FLANG_TIMES[@]}")"
awk -v a="${mean_fp}" -v b="${mean_fl}" '
    BEGIN {
        if (a > 0 && b > 0) {
            if (b < a) printf "\nflang is %.2fx faster than fparser2 (mean %.3fs vs %.3fs)\n", a / b, b, a
            else       printf "\nfparser2 is %.2fx faster than flang (mean %.3fs vs %.3fs)\n", b / a, a, b
        }
    }'

# ---------------------------------------------------------------------------
# Optional: cross-check the first-run generated outputs
# ---------------------------------------------------------------------------

fparser2_dir="${TMP_ROOT}/fparser2_r1"
flang_dir="${TMP_ROOT}/flang_r1"

echo
echo "------------------------------------------------------------------------"
echo "Output cross-check (first run of each backend)"
echo "------------------------------------------------------------------------"
if [[ -d "${fparser2_dir}" && -d "${flang_dir}" ]]; then
    diff_out="$(diff -rq \
                    --exclude=stdout.log --exclude=stderr.log \
                    "${fparser2_dir}" "${flang_dir}" 2>&1 || true)"
    if [[ -z "${diff_out}" ]]; then
        echo "Generated output directories are byte-identical."
    else
        echo "Differences found:"
        echo "${diff_out}"
    fi
else
    echo "One or both output directories missing, skipping diff."
fi

if ((KEEP_TMP == 1)); then
    echo
    echo "Temporary outputs preserved at: ${TMP_ROOT}"
fi
