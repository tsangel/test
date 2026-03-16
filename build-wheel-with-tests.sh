#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${ROOT_DIR}"

if [[ -z "${PYTHON_BIN:-}" ]]; then
	if command -v python3 >/dev/null 2>&1; then
		PYTHON_BIN="python3"
	elif command -v python >/dev/null 2>&1; then
		PYTHON_BIN="python"
	else
		echo "Error: neither python3 nor python is available on PATH." >&2
		exit 1
	fi
fi

: "${DICOMSDL_WHEEL_PROFILE:=static}"
: "${BUILD_TESTING:=ON}"
: "${DICOM_BUILD_EXAMPLES:=OFF}"
: "${RUN_TESTS:=1}"
: "${BUILD_WHEEL:=1}"
: "${WHEEL_ONLY:=0}"
: "${PIP_WHEEL_VERBOSE:=1}"
: "${PYTEST_ARGS:=}"

case "${DICOMSDL_WHEEL_PROFILE}" in
	static)
		wrapper_script="${ROOT_DIR}/build-wheel-static.sh"
		: "${BUILD_DIR:=${ROOT_DIR}/build-wheel-static-with-tests}"
		: "${WHEEL_DIR:=${ROOT_DIR}/dist-static-with-tests}"
		;;
	shared)
		wrapper_script="${ROOT_DIR}/build-wheel-shared.sh"
		: "${BUILD_DIR:=${ROOT_DIR}/build-wheel-shared-with-tests}"
		: "${WHEEL_DIR:=${ROOT_DIR}/dist-shared-with-tests}"
		;;
	*)
		echo "Error: DICOMSDL_WHEEL_PROFILE must be one of static|shared (got: ${DICOMSDL_WHEEL_PROFILE})." >&2
		exit 1
		;;
esac

ensure_python_test_requirements() {
	local py_bin="$1"
	if "$py_bin" - <<'PY' >/dev/null 2>&1
import importlib.util
import sys
missing = [m for m in ("pytest",) if importlib.util.find_spec(m) is None]
raise SystemExit(0 if not missing else 1)
PY
	then
		return 0
	fi

	echo "Error: pytest is required for wheel-with-tests validation with ${py_bin}." >&2
	echo "Run: ${py_bin} -m pip install --upgrade pytest" >&2
	exit 1
}

export PYTHON_BIN
export BUILD_DIR
export WHEEL_DIR
export BUILD_TESTING
export DICOM_BUILD_EXAMPLES
export RUN_TESTS
export BUILD_WHEEL
export WHEEL_ONLY
export PIP_WHEEL_VERBOSE

echo "Running wheel build with tests (profile=${DICOMSDL_WHEEL_PROFILE})"
"${wrapper_script}" "$@"

ensure_python_test_requirements "${PYTHON_BIN}"

pytest_cmd=("${PYTHON_BIN}" -m pytest -q tests/python)
if [[ -n "${PYTEST_ARGS}" ]]; then
	# shellcheck disable=SC2206
	extra_pytest_args=(${PYTEST_ARGS})
	pytest_cmd+=("${extra_pytest_args[@]}")
fi

echo "Running Python test suite"
"${pytest_cmd[@]}"
