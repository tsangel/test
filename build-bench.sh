#!/usr/bin/env bash
# Build C++ benchmark (read_all_dcm) in RelWithDebInfo with debug info.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"
OUT_DIR="${ROOT_DIR}/build.bench"
CONFIG=RelWithDebInfo

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
	-DCMAKE_BUILD_TYPE=${CONFIG} \
	-DBUILD_SHARED_LIBS=OFF
cmake --build "${BUILD_DIR}" --config ${CONFIG}

lib_candidates=(
	"${BUILD_DIR}/libdicomsdl.a"
	"${BUILD_DIR}/libdicomsdl.dylib"
	"${BUILD_DIR}/libdicomsdl.so"
	"${BUILD_DIR}/${CONFIG}/dicomsdl.lib"
)

LIB_PATH=""
for cand in "${lib_candidates[@]}"; do
	if [[ -f "${cand}" ]]; then
		LIB_PATH="${cand}"
		break
	fi
done

if [[ -z "${LIB_PATH}" ]]; then
	echo "libdicomsdl not found under ${BUILD_DIR}" >&2
	exit 1
fi

CXX_BIN="${CXX:-c++}"
OUT_BIN="${OUT_DIR}/read_all_dcm"

mkdir -p "${OUT_DIR}"

echo "Using lib: ${LIB_PATH}"
echo "Building ${OUT_BIN}"

"${CXX_BIN}" -std=c++20 -O3 -g -DNDEBUG \
	-I"${ROOT_DIR}/include" \
	-I"${BUILD_DIR}" \
	-I"${BUILD_DIR}/generated/include" \
	"${ROOT_DIR}/benchmarks/read_all_dcm.cpp" "${LIB_PATH}" \
	-Wl,-rpath,"$(dirname "${LIB_PATH}")" \
	-o "${OUT_BIN}"

echo "Done -> ${OUT_BIN}"
