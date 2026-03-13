#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BENCH_SCRIPT="${SCRIPT_DIR}/benchmark_multiframe_thread_matrix.py"

PYTHON_BIN="${PYTHON_BIN:-}"
WORKER_THREADS="${WORKER_THREADS:-1,2,4,8,all}"
CODEC_THREADS="${CODEC_THREADS:-1,2}"
WARMUP="${WARMUP:-1}"
REPEAT="${REPEAT:-4}"
TARGET_SAMPLE_MS="${TARGET_SAMPLE_MS:-150}"
MAX_INNER_LOOPS="${MAX_INNER_LOOPS:-128}"
HTJ2K_BACKEND="${HTJ2K_BACKEND:-openjph}"

resolve_python() {
  if [[ -n "${PYTHON_BIN}" ]]; then
    [[ -x "${PYTHON_BIN}" ]] || {
      echo "ERROR: Python not executable: ${PYTHON_BIN}" >&2
      exit 1
    }
    return
  fi

  local candidates=("python3" "python")
  local py
  for py in "${candidates[@]}"; do
    command -v "${py}" >/dev/null 2>&1 || continue
    PYTHON_BIN="${py}"
    return
  done

  echo "ERROR: Could not find Python. Set PYTHON_BIN." >&2
  exit 1
}

resolve_python

if [[ $# -eq 0 ]]; then
  echo "Usage: $(basename "$0") INPUT1 [INPUT2 ...] [extra benchmark args]" >&2
  echo >&2
  echo "Environment defaults:" >&2
  echo "  PYTHON_BIN=${PYTHON_BIN}" >&2
  echo "  WORKER_THREADS=${WORKER_THREADS}" >&2
  echo "  CODEC_THREADS=${CODEC_THREADS}" >&2
  echo "  WARMUP=${WARMUP}" >&2
  echo "  REPEAT=${REPEAT}" >&2
  echo "  TARGET_SAMPLE_MS=${TARGET_SAMPLE_MS}" >&2
  echo "  MAX_INNER_LOOPS=${MAX_INNER_LOOPS}" >&2
  echo "  HTJ2K_BACKEND=${HTJ2K_BACKEND}" >&2
  echo >&2
  echo "Example:" >&2
  echo "  $(basename "$0") ../sample/multiframe/multiframe.dcm --transfer-syntax JPEG2000" >&2
  echo >&2
  exec "${PYTHON_BIN}" "${BENCH_SCRIPT}" --help
fi

echo "Using Python: ${PYTHON_BIN}"
echo "WORKER_THREADS/CODEC_THREADS: ${WORKER_THREADS} / ${CODEC_THREADS}"
echo "WARMUP/REPEAT: ${WARMUP} / ${REPEAT}"
echo "HTJ2K_BACKEND: ${HTJ2K_BACKEND}"
echo

exec "${PYTHON_BIN}" "${BENCH_SCRIPT}" \
  --worker-threads "${WORKER_THREADS}" \
  --codec-threads "${CODEC_THREADS}" \
  --warmup "${WARMUP}" \
  --repeat "${REPEAT}" \
  --target-sample-ms "${TARGET_SAMPLE_MS}" \
  --max-inner-loops "${MAX_INNER_LOOPS}" \
  --htj2k-backend "${HTJ2K_BACKEND}" \
  "$@"
