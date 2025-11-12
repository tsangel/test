#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
CMAKE_GENERATOR="${CMAKE_GENERATOR:-Ninja}"
DICOM_BUILD_EXAMPLES="${DICOM_BUILD_EXAMPLES:-ON}"

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
