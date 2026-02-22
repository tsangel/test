#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
PYTHON_BIN="${PYTHON_BIN:-python3}"
MODULE_NAME="${MODULE_NAME:-dicomsdl._dicomsdl}"
OUTPUT_PATH="${OUTPUT_PATH:-${ROOT_DIR}/bindings/python/stubs/_dicomsdl.stubgen.pyi}"
CHECK_ONLY=0
QUIET=0

usage() {
	cat <<'USAGE'
Usage: scripts/update_stub.sh [options]

Generate nanobind stub snapshot for dicomsdl Python bindings.

Options:
  --build-dir PATH     Build directory containing _dicomsdl extension
  --python BIN         Python interpreter to run stubgen
  --module NAME        Module name to generate stubs for (default: dicomsdl._dicomsdl)
  --output PATH        Output .pyi snapshot path
  --check              Check mode: fail if generated snapshot differs
  --quiet              Suppress stubgen logs
  -h, --help           Show this help
USAGE
}

while [[ $# -gt 0 ]]; do
	case "$1" in
	--build-dir)
		BUILD_DIR="$2"
		shift 2
		;;
	--python)
		PYTHON_BIN="$2"
		shift 2
		;;
	--module)
		MODULE_NAME="$2"
		shift 2
		;;
	--output)
		OUTPUT_PATH="$2"
		shift 2
		;;
	--check)
		CHECK_ONLY=1
		shift
		;;
	--quiet)
		QUIET=1
		shift
		;;
	-h | --help)
		usage
		exit 0
		;;
	*)
		echo "Unknown option: $1" >&2
		usage >&2
		exit 2
		;;
	esac
done

if ! command -v "$PYTHON_BIN" >/dev/null 2>&1; then
	echo "Error: python interpreter not found: ${PYTHON_BIN}" >&2
	exit 1
fi

STUBGEN_SCRIPT="${ROOT_DIR}/extern/nanobind/src/stubgen.py"
if [[ ! -f "$STUBGEN_SCRIPT" ]]; then
	echo "Error: nanobind stubgen script not found: ${STUBGEN_SCRIPT}" >&2
	exit 1
fi

if [[ ! -d "$BUILD_DIR" ]]; then
	echo "Error: build directory not found: ${BUILD_DIR}" >&2
	exit 1
fi

TMP_STUB="$(mktemp "${TMPDIR:-/tmp}/dicomsdl_stubgen.XXXXXX.pyi")"
trap 'rm -f "$TMP_STUB"' EXIT

cmd=(
	"$PYTHON_BIN" "$STUBGEN_SCRIPT"
	-i "$BUILD_DIR"
	-i "${ROOT_DIR}/bindings/python"
	-m "$MODULE_NAME"
	-o "$TMP_STUB"
	--exclude-values
	--exclude-docstrings
)

if [[ "$QUIET" -eq 1 ]]; then
	cmd+=(-q)
fi

"${cmd[@]}"

if [[ "$CHECK_ONLY" -eq 1 ]]; then
	if [[ ! -f "$OUTPUT_PATH" ]]; then
		echo "Error: snapshot file not found: ${OUTPUT_PATH}" >&2
		echo "Run scripts/update_stub.sh first to create it." >&2
		exit 1
	fi

	if ! diff -u "$OUTPUT_PATH" "$TMP_STUB" >/dev/null; then
		echo "Stub snapshot is out of date: ${OUTPUT_PATH}" >&2
		echo "Run: scripts/update_stub.sh --build-dir ${BUILD_DIR}" >&2
		exit 1
	fi
	echo "Stub snapshot is up to date: ${OUTPUT_PATH}"
	exit 0
fi

mkdir -p "$(dirname "$OUTPUT_PATH")"
if [[ -f "$OUTPUT_PATH" ]] && cmp -s "$OUTPUT_PATH" "$TMP_STUB"; then
	echo "No changes: ${OUTPUT_PATH}"
	exit 0
fi

mv "$TMP_STUB" "$OUTPUT_PATH"
echo "Updated: ${OUTPUT_PATH}"
