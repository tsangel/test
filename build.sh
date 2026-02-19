#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
BUILD_TESTING="${BUILD_TESTING:-ON}"
DICOM_BUILD_EXAMPLES="${DICOM_BUILD_EXAMPLES:-ON}"
BUILD_WHEEL="${BUILD_WHEEL:-1}"
CTEST_LABEL="${CTEST_LABEL:-dicomsdl}"
PYTHON_BIN="${PYTHON_BIN:-python3}"
WHEEL_DIR="${WHEEL_DIR:-${ROOT_DIR}/dist}"

if ! command -v cmake >/dev/null 2>&1; then
	echo "Error: cmake is not installed or not on PATH" >&2
	exit 1
fi

if [[ -n "${CMAKE_GENERATOR:-}" ]]; then
	REQUESTED_GENERATOR="$CMAKE_GENERATOR"
	GENERATOR_EXPLICIT=1
else
	if command -v ninja >/dev/null 2>&1; then
		REQUESTED_GENERATOR="Ninja"
	else
		REQUESTED_GENERATOR="Unix Makefiles"
	fi
	GENERATOR_EXPLICIT=0
fi

CMAKE_CACHE_FILE="${BUILD_DIR}/CMakeCache.txt"
if [[ -f "$CMAKE_CACHE_FILE" ]]; then
	EXISTING_GENERATOR="$(sed -n 's/^CMAKE_GENERATOR:INTERNAL=//p' "$CMAKE_CACHE_FILE" | head -n 1)"
	if [[ -n "$EXISTING_GENERATOR" && "$EXISTING_GENERATOR" != "$REQUESTED_GENERATOR" ]]; then
		if (( GENERATOR_EXPLICIT )); then
			if [[ "${RESET_CMAKE_CACHE:-0}" == "1" ]]; then
				echo "Resetting CMake cache in ${BUILD_DIR} to switch generator (${EXISTING_GENERATOR} -> ${REQUESTED_GENERATOR})"
				rm -rf "${BUILD_DIR}/CMakeCache.txt" "${BUILD_DIR}/CMakeFiles"
			else
				echo "Error: ${BUILD_DIR} was configured with generator '${EXISTING_GENERATOR}', but requested '${REQUESTED_GENERATOR}'." >&2
				echo "       Set RESET_CMAKE_CACHE=1 to remove CMakeCache.txt/CMakeFiles automatically." >&2
				exit 1
			fi
		else
			echo "Using existing CMake generator '${EXISTING_GENERATOR}' from ${BUILD_DIR}"
			REQUESTED_GENERATOR="$EXISTING_GENERATOR"
		fi
	fi
fi

cmake_args=(-S "$ROOT_DIR" -B "$BUILD_DIR" \
	-DBUILD_TESTING="${BUILD_TESTING}" \
	-DDICOM_BUILD_EXAMPLES="${DICOM_BUILD_EXAMPLES}" \
	-DCMAKE_BUILD_TYPE="${BUILD_TYPE}")

if [[ -n "$REQUESTED_GENERATOR" ]]; then
	cmake_args+=(-G "$REQUESTED_GENERATOR")
fi

echo "Configuring dicomsdl (${BUILD_TYPE}) in $BUILD_DIR"
cmake "${cmake_args[@]}"

if [[ -z "${BUILD_PARALLELISM:-}" ]]; then
	if command -v sysctl >/dev/null 2>&1 && BUILD_PARALLELISM="$(sysctl -n hw.ncpu 2>/dev/null)"; then
		:
	elif command -v getconf >/dev/null 2>&1 && BUILD_PARALLELISM="$(getconf _NPROCESSORS_ONLN 2>/dev/null)"; then
		:
	else
		BUILD_PARALLELISM=1
	fi
fi

if [[ ! "$BUILD_PARALLELISM" =~ ^[0-9]+$ ]] || (( BUILD_PARALLELISM < 1 )); then
	BUILD_PARALLELISM=1
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
	if [[ -n "${CTEST_LABEL}" ]]; then
		ctest --output-on-failure --build-config "$BUILD_TYPE" -L "$CTEST_LABEL"
	else
		ctest --output-on-failure --build-config "$BUILD_TYPE"
	fi
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
