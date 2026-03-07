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
: "${BUILD_DIR:=${ROOT_DIR}/build-wheel-static}"
: "${WHEEL_DIR:=${ROOT_DIR}/dist-static}"
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
: "${AUTO_INSTALL_PYTHON_WHEEL_DEPS:=1}"

: "${DICOMSDL_PIXEL_DEFAULT_MODE:=none}"
: "${DICOMSDL_PIXEL_JPEG_MODE:=builtin}"
: "${DICOMSDL_PIXEL_JPEGLS_MODE:=builtin}"
: "${DICOMSDL_PIXEL_JPEG2K_MODE:=builtin}"
: "${DICOMSDL_PIXEL_HTJ2K_MODE:=builtin}"
: "${DICOMSDL_PIXEL_JPEGXL_MODE:=builtin}"

static_args="-DDICOM_BUILD_PYTHON=OFF -DDICOMSDL_ENABLE_JPEGXL=ON -DDICOMSDL_PIXEL_OPENJPEG_PLUGIN=OFF -DDICOMSDL_PIXEL_JPEG_PLUGIN=OFF -DDICOMSDL_PIXEL_JPEGLS_PLUGIN=OFF -DDICOMSDL_PIXEL_HTJ2K_PLUGIN=OFF -DDICOMSDL_PIXEL_JPEGXL_PLUGIN=OFF -DDICOMSDL_PIXEL_RLE_STATIC_PLUGIN=ON -DDICOMSDL_PIXEL_OPENJPEG_STATIC_PLUGIN=ON -DDICOMSDL_PIXEL_JPEG_STATIC_PLUGIN=ON -DDICOMSDL_PIXEL_JPEGLS_STATIC_PLUGIN=ON -DDICOMSDL_PIXEL_HTJ2K_STATIC_PLUGIN=ON -DDICOMSDL_PIXEL_JPEGXL_STATIC_PLUGIN=ON"
static_wheel_args="-DDICOMSDL_ENABLE_JPEGXL=ON -DDICOMSDL_PIXEL_OPENJPEG_PLUGIN=OFF -DDICOMSDL_PIXEL_JPEG_PLUGIN=OFF -DDICOMSDL_PIXEL_JPEGLS_PLUGIN=OFF -DDICOMSDL_PIXEL_HTJ2K_PLUGIN=OFF -DDICOMSDL_PIXEL_JPEGXL_PLUGIN=OFF -DDICOMSDL_PIXEL_RLE_STATIC_PLUGIN=ON -DDICOMSDL_PIXEL_OPENJPEG_STATIC_PLUGIN=ON -DDICOMSDL_PIXEL_JPEG_STATIC_PLUGIN=ON -DDICOMSDL_PIXEL_JPEGLS_STATIC_PLUGIN=ON -DDICOMSDL_PIXEL_HTJ2K_STATIC_PLUGIN=ON -DDICOMSDL_PIXEL_JPEGXL_STATIC_PLUGIN=ON"

: "${CMAKE_EXTRA_ARGS:=${static_args}}"
: "${DICOMSDL_CMAKE_ARGS:=${static_wheel_args}}"

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
export AUTO_INSTALL_PYTHON_WHEEL_DEPS
export DICOMSDL_PIXEL_DEFAULT_MODE
export DICOMSDL_PIXEL_JPEG_MODE
export DICOMSDL_PIXEL_JPEGLS_MODE
export DICOMSDL_PIXEL_JPEG2K_MODE
export DICOMSDL_PIXEL_HTJ2K_MODE
export DICOMSDL_PIXEL_JPEGXL_MODE
export CMAKE_EXTRA_ARGS
export DICOMSDL_CMAKE_ARGS

ensure_python_wheel_build_requirements() {
	local py_bin="$1"
	if "$py_bin" - <<'PY' >/dev/null 2>&1
import importlib.util
import sys
missing = [m for m in ("pip", "setuptools.build_meta", "wheel") if importlib.util.find_spec(m) is None]
raise SystemExit(0 if not missing else 1)
PY
	then
		return 0
	fi

	echo "Python wheel build dependencies are missing for ${py_bin} (pip/setuptools/wheel)." >&2
	if [[ "${AUTO_INSTALL_PYTHON_WHEEL_DEPS}" == "0" ]]; then
		echo "Run: ${py_bin} -m pip install --upgrade pip setuptools wheel" >&2
		exit 1
	fi

	echo "Attempting to install missing wheel build dependencies..." >&2
	"$py_bin" -m ensurepip --upgrade >/dev/null 2>&1 || true
	if ! "$py_bin" -m pip install --upgrade pip setuptools wheel; then
		echo "Error: failed to install Python wheel build dependencies." >&2
		echo "Run manually: ${py_bin} -m pip install --upgrade pip setuptools wheel" >&2
		exit 1
	fi

	if ! "$py_bin" - <<'PY' >/dev/null 2>&1
import importlib.util
import sys
missing = [m for m in ("pip", "setuptools.build_meta", "wheel") if importlib.util.find_spec(m) is None]
raise SystemExit(0 if not missing else 1)
PY
	then
		echo "Error: pip/setuptools/wheel are still unavailable after installation attempt." >&2
		exit 1
	fi
}

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
	if [[ -e "${WHEEL_DIR}" && ! -d "${WHEEL_DIR}" ]]; then
		echo "Error: wheel directory path exists but is not a directory: ${WHEEL_DIR}" >&2
		exit 1
	fi
	mkdir -p "${WHEEL_DIR}"
fi

if [[ "${MSYSTEM:-}" == "CLANG64" ]]; then
	export CC="${CC:-clang}"
	export CXX="${CXX:-clang++}"
	if command -v ninja >/dev/null 2>&1; then
		export CMAKE_GENERATOR="${CMAKE_GENERATOR:-Ninja}"
	fi
fi

ensure_python_wheel_build_requirements "${PYTHON_BIN}"

"${ROOT_DIR}/build.sh" "$@"
