#!/usr/bin/env bash
if [ -z "${BASH_VERSION:-}" ]; then
	exec bash "$0" "$@"
fi

set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
BUILD_TESTING="${BUILD_TESTING:-ON}"
DICOM_BUILD_EXAMPLES="${DICOM_BUILD_EXAMPLES:-ON}"
DICOMSDL_PIXEL_DEFAULT_MODE="${DICOMSDL_PIXEL_DEFAULT_MODE:-builtin}"
# Keep JPEG XL opt-in by default because enabling JPEG XL modes requires libjxl.
DICOMSDL_PIXEL_JPEGXL_MODE="${DICOMSDL_PIXEL_JPEGXL_MODE:-none}"
BUILD_WHEEL="${BUILD_WHEEL:-1}"
CTEST_LABEL="${CTEST_LABEL:-dicomsdl}"
PYTHON_BIN="${PYTHON_BIN:-python3}"
WHEEL_DIR="${WHEEL_DIR:-${ROOT_DIR}/dist}"
PRESERVE_WHEEL_HISTORY="${PRESERVE_WHEEL_HISTORY:-1}"
pixel_v2_args=(
	-DDICOMSDL_PIXEL_CORE=ON
	-DDICOMSDL_PIXEL_RUNTIME=ON
	-DDICOMSDL_PIXEL_RLE_STATIC_PLUGIN=ON
	-DDICOMSDL_PIXEL_OPENJPEG_STATIC_PLUGIN=ON
	-DDICOMSDL_PIXEL_JPEG_STATIC_PLUGIN=ON
	-DDICOMSDL_PIXEL_JPEGLS_STATIC_PLUGIN=ON
	-DDICOMSDL_PIXEL_HTJ2K_STATIC_PLUGIN=ON
)

if ! command -v cmake >/dev/null 2>&1; then
	echo "Error: cmake is not installed or not on PATH" >&2
	exit 1
fi

default_mode_lc="$(printf '%s' "$DICOMSDL_PIXEL_DEFAULT_MODE" | tr '[:upper:]' '[:lower:]')"
case "$default_mode_lc" in
	builtin|shared|none) ;;
	*)
		echo "Error: DICOMSDL_PIXEL_DEFAULT_MODE must be one of builtin|shared|none (got: ${DICOMSDL_PIXEL_DEFAULT_MODE})" >&2
		exit 1
		;;
esac

CODEC_LIST=(JPEG JPEGLS JPEG2K HTJ2K JPEGXL)
codec_pixel_args=()
codec_mode_log_parts=()
jpegxl_mode_requested=0

for codec in "${CODEC_LIST[@]}"; do
	mode_var="DICOMSDL_PIXEL_${codec}_MODE"
	mode_value="${!mode_var:-$default_mode_lc}"
	mode_value_lc="$(printf '%s' "$mode_value" | tr '[:upper:]' '[:lower:]')"

	case "$codec" in
		JPEG)
			static_opt="DICOMSDL_PIXEL_JPEG_STATIC_PLUGIN"
			shared_opt="DICOMSDL_PIXEL_JPEG_PLUGIN"
			;;
		JPEGLS)
			static_opt="DICOMSDL_PIXEL_JPEGLS_STATIC_PLUGIN"
			shared_opt="DICOMSDL_PIXEL_JPEGLS_PLUGIN"
			;;
		JPEG2K)
			static_opt="DICOMSDL_PIXEL_OPENJPEG_STATIC_PLUGIN"
			shared_opt="DICOMSDL_PIXEL_OPENJPEG_PLUGIN"
			;;
		HTJ2K)
			static_opt="DICOMSDL_PIXEL_HTJ2K_STATIC_PLUGIN"
			shared_opt="DICOMSDL_PIXEL_HTJ2K_PLUGIN"
			;;
		JPEGXL)
			static_opt="DICOMSDL_PIXEL_JPEGXL_STATIC_PLUGIN"
			shared_opt="DICOMSDL_PIXEL_JPEGXL_PLUGIN"
			;;
		*)
			echo "Error: unsupported codec key '${codec}'" >&2
			exit 1
			;;
	esac

	case "$mode_value_lc" in
		builtin)
			codec_pixel_args+=("-D${static_opt}=ON")
			codec_pixel_args+=("-D${shared_opt}=OFF")
			if [[ "$codec" == "JPEGXL" ]]; then
				jpegxl_mode_requested=1
			fi
			;;
		shared)
			codec_pixel_args+=("-D${static_opt}=OFF")
			codec_pixel_args+=("-D${shared_opt}=ON")
			if [[ "$codec" == "JPEGXL" ]]; then
				jpegxl_mode_requested=1
			fi
			;;
		none)
			codec_pixel_args+=("-D${static_opt}=OFF")
			codec_pixel_args+=("-D${shared_opt}=OFF")
			;;
		*)
			echo "Error: ${mode_var} must be one of builtin|shared|none (got: ${mode_value})" >&2
			exit 1
			;;
	esac

	codec_label_lc="$(printf '%s' "$codec" | tr '[:upper:]' '[:lower:]')"
	codec_mode_log_parts+=("${codec_label_lc}=${mode_value_lc}")
done

if [[ -n "${CMAKE_GENERATOR:-}" ]]; then
	REQUESTED_GENERATOR="$CMAKE_GENERATOR"
	GENERATOR_EXPLICIT=1
else
	if command -v ninja >/dev/null 2>&1; then
		ninja_bin="$(command -v ninja)"
		if [[ "$(uname -s)" == "Darwin" && "$(uname -m)" == "arm64" ]] && command -v lipo >/dev/null 2>&1; then
			ninja_archs="$(lipo -archs "$ninja_bin" 2>/dev/null || true)"
			if [[ "$ninja_archs" == *arm64* ]]; then
				REQUESTED_GENERATOR="Ninja"
			else
				echo "Detected non-arm64 ninja '${ninja_bin}'; falling back to Unix Makefiles"
				REQUESTED_GENERATOR="Unix Makefiles"
			fi
		else
			REQUESTED_GENERATOR="Ninja"
		fi
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

if ((${#codec_pixel_args[@]})); then
	cmake_args+=("${codec_pixel_args[@]}")
fi
if ((jpegxl_mode_requested)); then
	cmake_args+=("-DDICOMSDL_ENABLE_JPEGXL=ON")
fi
if ((${#pixel_v2_args[@]})); then
	cmake_args+=("${pixel_v2_args[@]}")
fi

if [[ -n "${CMAKE_EXTRA_ARGS:-}" ]]; then
	# shellcheck disable=SC2206
	extra_cmake_args=(${CMAKE_EXTRA_ARGS})
	cmake_args+=("${extra_cmake_args[@]}")
fi

if [[ -n "$REQUESTED_GENERATOR" ]]; then
	cmake_args+=(-G "$REQUESTED_GENERATOR")
fi

echo "Configuring dicomsdl (${BUILD_TYPE}) in $BUILD_DIR"
echo "Codec modes: ${codec_mode_log_parts[*]}"
echo "Pixel runtime mode: v2 (default)"
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

case "$BUILD_PARALLELISM" in
	''|*[!0-9]*)
		BUILD_PARALLELISM=1
		;;
	*)
		if [ "$BUILD_PARALLELISM" -lt 1 ]; then
			BUILD_PARALLELISM=1
		fi
		;;
esac

build_cmd=(cmake --build "$BUILD_DIR" --config "$BUILD_TYPE" --parallel "$BUILD_PARALLELISM")
if [ "$#" -gt 0 ]; then
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
	tmp_wheel_dir="$(mktemp -d "${WHEEL_DIR}/.wheel-build.XXXXXX")"
	"$PYTHON_BIN" -m pip wheel "$ROOT_DIR" --no-build-isolation --no-deps -w "$tmp_wheel_dir"
	wheel_timestamp="$(date '+%Y%m%d-%H%M%S')"
	shopt -s nullglob
	built_wheels=("$tmp_wheel_dir"/*.whl)
	if ((${#built_wheels[@]} == 0)); then
		echo "Error: wheel build did not produce any .whl files in ${tmp_wheel_dir}" >&2
		rm -rf "$tmp_wheel_dir"
		exit 1
	fi
	for built_wheel in "${built_wheels[@]}"; do
		wheel_name="$(basename "$built_wheel")"
		target_wheel="${WHEEL_DIR}/${wheel_name}"
		if [[ -f "$target_wheel" && "${PRESERVE_WHEEL_HISTORY}" != "0" ]]; then
			backup_wheel="${WHEEL_DIR}/${wheel_name%.whl}-prev-${wheel_timestamp}.whl"
			backup_suffix=1
			while [[ -e "$backup_wheel" ]]; do
				backup_wheel="${WHEEL_DIR}/${wheel_name%.whl}-prev-${wheel_timestamp}-${backup_suffix}.whl"
				backup_suffix=$((backup_suffix + 1))
			done
			mv "$target_wheel" "$backup_wheel"
			echo "Archived existing wheel: ${target_wheel} -> ${backup_wheel}"
		fi
		mv "$built_wheel" "$target_wheel"
		echo "Saved wheel: ${target_wheel}"
	done
	shopt -u nullglob
	rm -rf "$tmp_wheel_dir"
fi
