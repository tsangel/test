#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if ! command -v git >/dev/null 2>&1; then
	echo "Error: git is not installed or not on PATH." >&2
	exit 1
fi

cd "$ROOT_DIR"

if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
	echo "Error: this script must run inside a git repository." >&2
	exit 1
fi

if [[ ! -f .gitmodules ]] || ! git config -f .gitmodules --get submodule.extern/libjxl.path >/dev/null 2>&1; then
	echo "Error: extern/libjxl is not declared in .gitmodules." >&2
	echo "Run this first:" >&2
	echo "  git -c submodule.recurse=false submodule add --depth 1 https://github.com/libjxl/libjxl.git extern/libjxl" >&2
	exit 1
fi

echo "Initializing extern/libjxl (shallow)..."
git submodule update --init --depth 1 extern/libjxl

disabled_nested_submodules=(
	testdata
	third_party/googletest
	third_party/lcms
	third_party/libjpeg-turbo
	third_party/libpng
	third_party/sjpeg
	third_party/zlib
)

for nested in "${disabled_nested_submodules[@]}"; do
	git -C extern/libjxl config "submodule.${nested}.update" none
done

echo "Initializing required nested submodules only..."
git -C extern/libjxl submodule update --init --depth 1 \
	third_party/highway \
	third_party/brotli \
	third_party/skcms

echo
echo "Current extern/libjxl nested submodule status:"
git -C extern/libjxl submodule status

