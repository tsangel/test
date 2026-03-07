#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${ROOT_DIR}"

: "${PYTHON_BIN:=python3}"
: "${BUILD_DIR:=${ROOT_DIR}/build-wheel-shared}"
: "${WHEEL_DIR:=${ROOT_DIR}/dist-shared}"
: "${CLEAN_BUILD_DIR:=1}"
: "${RESET_CMAKE_CACHE:=1}"
: "${BUILD_TESTING:=OFF}"
: "${DICOM_BUILD_EXAMPLES:=OFF}"
: "${RUN_TESTS:=0}"
: "${BUILD_WHEEL:=1}"
: "${FORCE_WHEEL_RELEASE:=1}"
: "${BUILD_TYPE:=Release}"
: "${DEBUG:=0}"
: "${DISTUTILS_DEBUG:=0}"
: "${STATIC_PRE_CLEAN_OUTPUTS:=1}"
: "${DICOMSDL_PIXEL_DEFAULT_MODE:=none}"
: "${DICOMSDL_PIXEL_JPEG_MODE:=shared}"
: "${DICOMSDL_PIXEL_JPEGLS_MODE:=shared}"
: "${DICOMSDL_PIXEL_JPEG2K_MODE:=shared}"
: "${DICOMSDL_PIXEL_HTJ2K_MODE:=shared}"
: "${DICOMSDL_PIXEL_JPEGXL_MODE:=shared}"

shared_args="-DDICOM_BUILD_PYTHON=OFF -DDICOMSDL_PIXEL_RLE_STATIC_PLUGIN=OFF"
shared_wheel_args="-DDICOMSDL_ENABLE_JPEGXL=ON -DDICOMSDL_PIXEL_RLE_STATIC_PLUGIN=OFF -DDICOMSDL_PIXEL_OPENJPEG_STATIC_PLUGIN=OFF -DDICOMSDL_PIXEL_JPEG_STATIC_PLUGIN=OFF -DDICOMSDL_PIXEL_JPEGLS_STATIC_PLUGIN=OFF -DDICOMSDL_PIXEL_HTJ2K_STATIC_PLUGIN=OFF -DDICOMSDL_PIXEL_JPEGXL_STATIC_PLUGIN=OFF -DDICOMSDL_PIXEL_OPENJPEG_PLUGIN=ON -DDICOMSDL_PIXEL_JPEG_PLUGIN=ON -DDICOMSDL_PIXEL_JPEGLS_PLUGIN=ON -DDICOMSDL_PIXEL_HTJ2K_PLUGIN=ON -DDICOMSDL_PIXEL_JPEGXL_PLUGIN=ON"

: "${CMAKE_EXTRA_ARGS:=${shared_args}}"
: "${DICOMSDL_CMAKE_ARGS:=${shared_wheel_args}}"

export PYTHON_BIN
export BUILD_DIR
export WHEEL_DIR
export CLEAN_BUILD_DIR
export RESET_CMAKE_CACHE
export BUILD_TESTING
export DICOM_BUILD_EXAMPLES
export RUN_TESTS
export BUILD_WHEEL
export FORCE_WHEEL_RELEASE
export BUILD_TYPE
export DEBUG
export DISTUTILS_DEBUG
export STATIC_PRE_CLEAN_OUTPUTS
export DICOMSDL_PIXEL_DEFAULT_MODE
export DICOMSDL_PIXEL_JPEG_MODE
export DICOMSDL_PIXEL_JPEGLS_MODE
export DICOMSDL_PIXEL_JPEG2K_MODE
export DICOMSDL_PIXEL_HTJ2K_MODE
export DICOMSDL_PIXEL_JPEGXL_MODE
export CMAKE_EXTRA_ARGS
export DICOMSDL_CMAKE_ARGS

normalize_dir_path() {
	local path="$1"
	while [[ "${path}" == */ && "${path}" != "/" ]]; do
		path="${path%/}"
	done
	printf "%s" "${path}"
}

safe_remove_dir_if_exists() {
	local target="$1"
	local label="$2"
	local normalized_target
	local normalized_root
	normalized_target="$(normalize_dir_path "${target}")"
	normalized_root="$(normalize_dir_path "${ROOT_DIR}")"
	if [[ -z "${normalized_target}" || "${normalized_target}" == "/" || "${normalized_target}" == "${normalized_root}" ]]; then
		echo "Error: refusing to remove unsafe ${label} path '${target}'" >&2
		exit 1
	fi
	if [[ -e "${normalized_target}" ]]; then
		if [[ ! -d "${normalized_target}" ]]; then
			echo "Error: ${label} exists but is not a directory: ${normalized_target}" >&2
			exit 1
		fi
		echo "Removing existing ${label}: ${normalized_target}"
		rm -rf "${normalized_target}"
	fi
}

if [[ "${STATIC_PRE_CLEAN_OUTPUTS}" != "0" ]]; then
	safe_remove_dir_if_exists "${BUILD_DIR}" "build directory"
	for path in "${ROOT_DIR}"/build/temp.* "${ROOT_DIR}"/build/lib.* "${ROOT_DIR}"/build/bdist.*; do
		if [[ -d "${path}" ]]; then
			echo "Removing wheel intermediate directory: ${path}"
			rm -rf "${path}"
		fi
	done
	safe_remove_dir_if_exists "${WHEEL_DIR}" "wheel directory"
	mkdir -p "${WHEEL_DIR}"
fi

"${ROOT_DIR}/build.sh" "$@"
