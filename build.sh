#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
CMAKE_GENERATOR="${CMAKE_GENERATOR:-Ninja}"
DICOM_BUILD_EXAMPLES="${DICOM_BUILD_EXAMPLES:-ON}"
BUILD_WHEEL="${BUILD_WHEEL:-1}"
PYTHON_BIN="${PYTHON_BIN:-python3}"
WHEEL_DIR="${WHEEL_DIR:-${ROOT_DIR}/dist}"

if ! command -v cmake >/dev/null 2>&1; then
	echo "Error: cmake is not installed or not on PATH" >&2
	exit 1
fi

cmake_args=(-S "$ROOT_DIR" -B "$BUILD_DIR" \
	-DDICOM_BUILD_EXAMPLES="${DICOM_BUILD_EXAMPLES}" \
	-DCMAKE_BUILD_TYPE="${BUILD_TYPE}")

if [[ -n "$CMAKE_GENERATOR" ]]; then
	cmake_args+=(-G "$CMAKE_GENERATOR")
fi

echo "Configuring dicomsdl (${BUILD_TYPE}) in $BUILD_DIR"
cmake "${cmake_args[@]}"

if [[ -z "${BUILD_PARALLELISM:-}" ]]; then
	if command -v sysctl >/dev/null 2>&1; then
		BUILD_PARALLELISM="$(sysctl -n hw.ncpu)"
	elif command -v getconf >/dev/null 2>&1; then
		BUILD_PARALLELISM="$(getconf _NPROCESSORS_ONLN)"
	else
		BUILD_PARALLELISM=1
	fi
fi

build_cmd=(cmake --build "$BUILD_DIR" --config "$BUILD_TYPE" --parallel "$BUILD_PARALLELISM")
if (( $# )); then
	build_cmd+=(--target)
	for target in "$@"; do
		build_cmd+=("$target")
	done
fi

echo "Building dicomsdl (${BUILD_TYPE})"
"${build_cmd[@]}"

if [[ "${RUN_TESTS:-1}" != "0" ]]; then
	echo "Running CTest suite (${BUILD_TYPE})"
	pushd "$BUILD_DIR" >/dev/null
	ctest --output-on-failure --build-config "$BUILD_TYPE"
	popd >/dev/null
fi

if [[ "${BUILD_WHEEL}" != "0" ]]; then
	if ! command -v "$PYTHON_BIN" >/dev/null 2>&1; then
		echo "Error: ${PYTHON_BIN} not found; set PYTHON_BIN to a valid interpreter." >&2
		exit 1
	fi
	echo "Building Python wheel into ${WHEEL_DIR}"
	mkdir -p "$WHEEL_DIR"
	"$PYTHON_BIN" -m pip wheel "$ROOT_DIR" --no-build-isolation --no-deps -w "$WHEEL_DIR"
fi
