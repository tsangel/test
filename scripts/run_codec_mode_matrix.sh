#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_ROOT="${BUILD_ROOT:-${ROOT_DIR}/build_codec_matrix}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
JOBS="${JOBS:-4}"
TEST_REGEX="${TEST_REGEX:-^(basic_smoke|codec_cycle_roundtrip|codec_error_boundary|codec_plugin_loader|codec_plugin_dynamic_load|codec_plugin_registration)$}"

DEFAULT_CASES=(
	"all_builtin"
	"all_none"
	"jpeg2k_builtin_only"
	"jpegxl_shared_only"
	"mixed_jpeg2k_builtin_jpegxl_shared_htj2k_none"
)

FULL_CASES=(
	"all_builtin"
	"all_none"
	"jpeg_builtin_only"
	"jpegls_builtin_only"
	"jpeg2k_builtin_only"
	"htj2k_builtin_only"
	"jpegxl_builtin_only"
	"jpeg_shared_only"
	"jpegls_shared_only"
	"jpeg2k_shared_only"
	"htj2k_shared_only"
	"jpegxl_shared_only"
	"mixed_jpeg2k_builtin_jpegxl_shared_htj2k_none"
)

print_usage() {
	cat <<EOF
Usage:
  $(basename "$0") [--full] [--case NAME ...] [options]

Options:
  --full                    Run the full case set.
  --case NAME               Run only the given case (repeatable).
  --build-root DIR          Override build root directory.
  --build-type TYPE         CMAKE_BUILD_TYPE (default: ${BUILD_TYPE}).
  --jobs N                  Build/test parallelism (default: ${JOBS}).
  --test-regex REGEX        ctest -R regex (default: ${TEST_REGEX}).
  --list                    Print available cases and exit.
  -h, --help                Show this help.

No arguments runs the default smoke matrix:
  ${DEFAULT_CASES[*]}
EOF
}

print_case_list() {
	echo "Default cases:"
	for case_name in "${DEFAULT_CASES[@]}"; do
		echo "  - ${case_name}"
	done
	echo
	echo "Full cases:"
	for case_name in "${FULL_CASES[@]}"; do
		echo "  - ${case_name}"
	done
}

is_valid_case() {
	local case_name="$1"
	for known in "${FULL_CASES[@]}"; do
		if [[ "${known}" == "${case_name}" ]]; then
			return 0
		fi
	done
	return 1
}

emit_all_none_flags() {
	for codec in JPEG JPEGLS JPEG2K HTJ2K JPEGXL; do
		echo "-DDICOMSDL_CODEC_${codec}_BUILTIN=OFF"
		echo "-DDICOMSDL_CODEC_${codec}_SHARED=OFF"
	done
}

emit_all_builtin_flags() {
	for codec in JPEG JPEGLS JPEG2K HTJ2K JPEGXL; do
		echo "-DDICOMSDL_CODEC_${codec}_BUILTIN=ON"
		echo "-DDICOMSDL_CODEC_${codec}_SHARED=OFF"
	done
}

emit_single_builtin_flags() {
	local target_codec="$1"
	for codec in JPEG JPEGLS JPEG2K HTJ2K JPEGXL; do
		if [[ "${codec}" == "${target_codec}" ]]; then
			echo "-DDICOMSDL_CODEC_${codec}_BUILTIN=ON"
			echo "-DDICOMSDL_CODEC_${codec}_SHARED=OFF"
		else
			echo "-DDICOMSDL_CODEC_${codec}_BUILTIN=OFF"
			echo "-DDICOMSDL_CODEC_${codec}_SHARED=OFF"
		fi
	done
}

emit_single_shared_flags() {
	local target_codec="$1"
	for codec in JPEG JPEGLS JPEG2K HTJ2K JPEGXL; do
		if [[ "${codec}" == "${target_codec}" ]]; then
			echo "-DDICOMSDL_CODEC_${codec}_BUILTIN=OFF"
			echo "-DDICOMSDL_CODEC_${codec}_SHARED=ON"
		else
			echo "-DDICOMSDL_CODEC_${codec}_BUILTIN=OFF"
			echo "-DDICOMSDL_CODEC_${codec}_SHARED=OFF"
		fi
	done
}

emit_case_flags() {
	local case_name="$1"
	case "${case_name}" in
		all_builtin)
			emit_all_builtin_flags
			;;
		all_none)
			emit_all_none_flags
			;;
		jpeg_builtin_only)
			emit_single_builtin_flags "JPEG"
			;;
		jpegls_builtin_only)
			emit_single_builtin_flags "JPEGLS"
			;;
		jpeg2k_builtin_only)
			emit_single_builtin_flags "JPEG2K"
			;;
		htj2k_builtin_only)
			emit_single_builtin_flags "HTJ2K"
			;;
		jpegxl_builtin_only)
			emit_single_builtin_flags "JPEGXL"
			;;
		jpeg_shared_only)
			emit_single_shared_flags "JPEG"
			;;
		jpegls_shared_only)
			emit_single_shared_flags "JPEGLS"
			;;
		jpeg2k_shared_only)
			emit_single_shared_flags "JPEG2K"
			;;
		htj2k_shared_only)
			emit_single_shared_flags "HTJ2K"
			;;
		jpegxl_shared_only)
			emit_single_shared_flags "JPEGXL"
			;;
		mixed_jpeg2k_builtin_jpegxl_shared_htj2k_none)
			emit_all_builtin_flags
			echo "-DDICOMSDL_CODEC_HTJ2K_BUILTIN=OFF"
			echo "-DDICOMSDL_CODEC_HTJ2K_SHARED=OFF"
			echo "-DDICOMSDL_CODEC_JPEGXL_BUILTIN=OFF"
			echo "-DDICOMSDL_CODEC_JPEGXL_SHARED=ON"
			;;
		*)
			echo "Unknown case: ${case_name}" >&2
			return 1
			;;
	esac
}

expected_shared_target() {
	local case_name="$1"
	case "${case_name}" in
		jpeg_shared_only)
			echo "dicomsdl_codec_plugin_jpeg"
			;;
		jpegls_shared_only)
			echo "dicomsdl_codec_plugin_jpegls"
			;;
		jpeg2k_shared_only)
			echo "dicomsdl_codec_plugin_jpeg2k"
			;;
		htj2k_shared_only)
			echo "dicomsdl_codec_plugin_htj2k"
			;;
		jpegxl_shared_only|mixed_jpeg2k_builtin_jpegxl_shared_htj2k_none)
			echo "dicomsdl_codec_plugin_jpegxl"
			;;
		*)
			echo ""
			;;
	esac
}

run_case() {
	local case_name="$1"
	local build_dir="${BUILD_ROOT}/${case_name}"
	local flags=()
	local flag
	local shared_target

	while IFS= read -r flag; do
		if [[ -n "${flag}" ]]; then
			flags+=("${flag}")
		fi
	done < <(emit_case_flags "${case_name}")

	echo "[codec-matrix] configure ${case_name}"
	cmake -E rm -rf "${build_dir}"
	cmake -S "${ROOT_DIR}" -B "${build_dir}" \
		-DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
		-DBUILD_TESTING=ON \
		-DDICOM_BUILD_EXAMPLES=OFF \
		-DDICOM_BUILD_PYTHON=OFF \
		"${flags[@]}"

	echo "[codec-matrix] build ${case_name}"
	cmake --build "${build_dir}" -j"${JOBS}"

	shared_target="$(expected_shared_target "${case_name}")"
	if [[ -n "${shared_target}" ]]; then
		echo "[codec-matrix] verify shared target ${shared_target}"
		cmake --build "${build_dir}" --target "${shared_target}" -j"${JOBS}"
	fi

	echo "[codec-matrix] test ${case_name}"
	ctest --test-dir "${build_dir}" --output-on-failure -R "${TEST_REGEX}" -j"${JOBS}"
}

main() {
	local run_full=0
	local selected_cases=()

	while [[ $# -gt 0 ]]; do
		case "$1" in
			--full)
				run_full=1
				shift
				;;
			--case)
				if [[ $# -lt 2 ]]; then
					echo "--case requires a value" >&2
					exit 2
				fi
				selected_cases+=("$2")
				shift 2
				;;
			--build-root)
				if [[ $# -lt 2 ]]; then
					echo "--build-root requires a value" >&2
					exit 2
				fi
				BUILD_ROOT="$2"
				shift 2
				;;
			--build-type)
				if [[ $# -lt 2 ]]; then
					echo "--build-type requires a value" >&2
					exit 2
				fi
				BUILD_TYPE="$2"
				shift 2
				;;
			--jobs)
				if [[ $# -lt 2 ]]; then
					echo "--jobs requires a value" >&2
					exit 2
				fi
				JOBS="$2"
				shift 2
				;;
			--test-regex)
				if [[ $# -lt 2 ]]; then
					echo "--test-regex requires a value" >&2
					exit 2
				fi
				TEST_REGEX="$2"
				shift 2
				;;
			--list)
				print_case_list
				exit 0
				;;
			-h|--help)
				print_usage
				exit 0
				;;
			*)
				selected_cases+=("$1")
				shift
				;;
		esac
	done

	if [[ ${#selected_cases[@]} -eq 0 ]]; then
		if [[ ${run_full} -eq 1 ]]; then
			selected_cases=("${FULL_CASES[@]}")
		else
			selected_cases=("${DEFAULT_CASES[@]}")
		fi
	fi

	for case_name in "${selected_cases[@]}"; do
		if ! is_valid_case "${case_name}"; then
			echo "Unknown case: ${case_name}" >&2
			echo "Use --list to see available cases." >&2
			exit 2
		fi
	done

	echo "[codec-matrix] build root: ${BUILD_ROOT}"
	echo "[codec-matrix] build type: ${BUILD_TYPE}"
	echo "[codec-matrix] jobs: ${JOBS}"
	echo "[codec-matrix] test regex: ${TEST_REGEX}"
	echo "[codec-matrix] cases: ${selected_cases[*]}"

	for case_name in "${selected_cases[@]}"; do
		run_case "${case_name}"
	done

	echo "[codec-matrix] completed"
}

main "$@"
