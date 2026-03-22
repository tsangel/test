#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 3 ]]; then
    echo "usage: $0 <wheel> <dest_dir> <delocate_archs>" >&2
    exit 2
fi

wheel="$1"
dest_dir="$2"
delocate_archs="$3"

tmpdir="$(mktemp -d)"
cleanup() {
    rm -rf "$tmpdir"
}
trap cleanup EXIT

wheel_root="$tmpdir/wheel"
mkdir -p "$wheel_root"
unzip -q "$wheel" -d "$wheel_root"

echo "::group::macOS wheel binary diagnostics"
echo "wheel=$wheel"
echo "dest_dir=$dest_dir"
echo "delocate_archs=$delocate_archs"

while IFS= read -r -d '' binary; do
    rel_path="${binary#"$wheel_root"/}"
    echo "----- $rel_path : otool -L -----"
    otool -L "$binary"

    if command -v vtool >/dev/null 2>&1; then
        echo "----- $rel_path : vtool -show-build -----"
        vtool -show-build "$binary" || true
    else
        echo "----- $rel_path : otool -l build version -----"
        otool -l "$binary" | sed -n '/LC_BUILD_VERSION/,/sdk/p;/LC_VERSION_MIN_MACOSX/,/sdk/p'
    fi
done < <(find "$wheel_root" -type f \( -name '*.so' -o -name '*.dylib' \) -print0)

echo "::endgroup::"

exec delocate-wheel --require-archs "$delocate_archs" -w "$dest_dir" -v "$wheel"
