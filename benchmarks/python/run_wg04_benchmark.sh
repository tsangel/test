#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
BENCH_SCRIPT="${SCRIPT_DIR}/benchmark_wg04_pixel_decode.py"
PRINT_SCRIPT="${SCRIPT_DIR}/print_wg04_tables.py"

WARMUP="${WARMUP:-1}"
REPEAT="${REPEAT:-3}"
WG04_ROOT="${WG04_ROOT:-}"
PYTHON_BIN="${PYTHON_BIN:-}"

MAIN_JSON="${MAIN_JSON:-${ROOT_DIR}/build/wg04_pixel_decode_compare_r3_htj2k.json}"
HTJ2K_OPENJPEG_JSON="${HTJ2K_OPENJPEG_JSON:-${ROOT_DIR}/build/wg04_htj2k_openjpeg_r3_toarray_postopt.json}"
HTJ2K_OPENJPH_JSON="${HTJ2K_OPENJPH_JSON:-${ROOT_DIR}/build/wg04_htj2k_openjph_r3_toarray_postopt.json}"

resolve_python() {
  if [[ -n "${PYTHON_BIN}" ]]; then
    [[ -x "${PYTHON_BIN}" ]] || {
      echo "ERROR: Python not executable: ${PYTHON_BIN}" >&2
      exit 1
    }
    return
  fi

  local candidates=(
    "${ROOT_DIR}/.venv312/bin/python"
    "${ROOT_DIR}/.venv/bin/python"
    "python3"
    "python"
  )
  local py
  for py in "${candidates[@]}"; do
    if [[ "${py}" == "python3" || "${py}" == "python" ]]; then
      command -v "${py}" >/dev/null 2>&1 || continue
      PYTHON_BIN="${py}"
      return
    fi
    [[ -x "${py}" ]] || continue
    PYTHON_BIN="${py}"
    return
  done

  echo "ERROR: Could not find Python. Set PYTHON_BIN." >&2
  exit 1
}

run_bench() {
  local output_json="$1"
  shift

  local -a cmd=("${PYTHON_BIN}" "${BENCH_SCRIPT}")
  if [[ -n "${WG04_ROOT}" ]]; then
    cmd+=("${WG04_ROOT}")
  fi
  cmd+=("$@" "--warmup" "${WARMUP}" "--repeat" "${REPEAT}" "--json" "${output_json}")

  echo "+ ${cmd[*]}"
  PYTHONPATH="${ROOT_DIR}/bindings/python${PYTHONPATH:+:${PYTHONPATH}}" "${cmd[@]}"
}

resolve_python

echo "Using Python: ${PYTHON_BIN}"
echo "WARMUP/REPEAT: ${WARMUP}/${REPEAT}"
if [[ -n "${WG04_ROOT}" ]]; then
  echo "WG04_ROOT: ${WG04_ROOT}"
fi

echo
echo "[1/4] Full WG04 benchmark (dicomsdl + pydicom)"
run_bench "${MAIN_JSON}" --backend both

echo
echo "[2/4] HTJ2K benchmark (openjpeg)"
run_bench "${HTJ2K_OPENJPEG_JSON}" \
  --backend dicomsdl \
  --codec htj2kll \
  --codec htj2kly \
  --dicomsdl-mode to_array \
  --dicomsdl-htj2k-decoder openjpeg

echo
echo "[3/4] HTJ2K benchmark (openjph)"
run_bench "${HTJ2K_OPENJPH_JSON}" \
  --backend dicomsdl \
  --codec htj2kll \
  --codec htj2kly \
  --dicomsdl-mode to_array \
  --dicomsdl-htj2k-decoder openjph

echo
echo "[4/4] Markdown tables"
PYTHONPATH="${ROOT_DIR}/bindings/python${PYTHONPATH:+:${PYTHONPATH}}" \
  "${PYTHON_BIN}" "${PRINT_SCRIPT}" \
  --main-json "${MAIN_JSON}" \
  --htj2k-openjpeg-json "${HTJ2K_OPENJPEG_JSON}" \
  --htj2k-openjph-json "${HTJ2K_OPENJPH_JSON}"

echo
echo "Done."
echo "  main JSON: ${MAIN_JSON}"
echo "  HTJ2K openjpeg JSON: ${HTJ2K_OPENJPEG_JSON}"
echo "  HTJ2K openjph JSON: ${HTJ2K_OPENJPH_JSON}"
