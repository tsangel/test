#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

CHARLS_REPO="extern/charls"
TAGS="2.4.2,2.3.4,2.2.1"
WORKDIR="/tmp/charls-attached-repro"
declare -a INPUTS=()

usage() {
  cat <<EOF
Usage: $0 --input <file.jls> [--input <file.jls> ...] [options]

Run CharLS regression matrix using already-extracted codestream files
(e.g. files directly attached in a GitHub issue).

Options:
  --input <path>        Codestream input file (.jls). Repeat this option for multiple files.
  --charls-repo <path>  Local CharLS git repository (default: extern/charls)
  --tags <a,b,c>        Comma-separated CharLS tags (default: 2.4.2,2.3.4,2.2.1)
  --workdir <path>      Working directory (default: /tmp/charls-attached-repro)
  -h, --help            Show this help
EOF
}

while (($#)); do
  case "$1" in
    --input) INPUTS+=("$2"); shift 2 ;;
    --charls-repo) CHARLS_REPO="$2"; shift 2 ;;
    --tags) TAGS="$2"; shift 2 ;;
    --workdir) WORKDIR="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage; exit 2 ;;
  esac
done

if [[ ${#INPUTS[@]} -eq 0 ]]; then
  echo "error: at least one --input is required" >&2
  usage
  exit 2
fi

for cmd in git cmake c++; do
  if ! command -v "$cmd" >/dev/null 2>&1; then
    echo "error: required command not found: $cmd" >&2
    exit 2
  fi
done

if [[ ! -d "$CHARLS_REPO/.git" && ! -f "$CHARLS_REPO/.git" ]]; then
  echo "error: not a git repository: $CHARLS_REPO" >&2
  exit 2
fi

declare -a RESOLVED_INPUTS=()
for in_path in "${INPUTS[@]}"; do
  resolved="$(cd "$(dirname "$in_path")" && pwd)/$(basename "$in_path")"
  if [[ ! -f "$resolved" ]]; then
    echo "error: input file not found: $in_path" >&2
    exit 2
  fi
  RESOLVED_INPUTS+=("$resolved")
done

mkdir -p "$WORKDIR"
IFS=',' read -r -a TAG_LIST <<< "$TAGS"

RESULTS_FILE="$WORKDIR/results.tsv"
: > "$RESULTS_FILE"

HARNESS_SRC="$SCRIPT_DIR/decode_with_charls.cpp"

for tag in "${TAG_LIST[@]}"; do
  tag_trimmed="$(echo "$tag" | tr -d '[:space:]')"
  if [[ -z "$tag_trimmed" ]]; then
    continue
  fi

  SRC_DIR="$WORKDIR/src-$tag_trimmed"
  BUILD_DIR="$WORKDIR/build-$tag_trimmed"
  BIN_PATH="$WORKDIR/decode-$tag_trimmed"

  rm -rf "$SRC_DIR" "$BUILD_DIR" "$BIN_PATH"
  mkdir -p "$SRC_DIR"

  echo "== Build CharLS $tag_trimmed =="
  git -C "$CHARLS_REPO" archive --format=tar "$tag_trimmed" | tar -xf - -C "$SRC_DIR"
  cmake -S "$SRC_DIR" -B "$BUILD_DIR" -DBUILD_SHARED_LIBS=OFF >/dev/null
  cmake --build "$BUILD_DIR" -j4 >/dev/null

  LIB_PATH="$(find "$BUILD_DIR" -name 'libcharls*.a' | head -n 1)"
  if [[ -z "$LIB_PATH" ]]; then
    echo "error: libcharls static library not found for tag $tag_trimmed" >&2
    exit 2
  fi

  c++ -std=c++20 -I"$SRC_DIR/include" "$HARNESS_SRC" "$LIB_PATH" -o "$BIN_PATH"

  for codestream in "${RESOLVED_INPUTS[@]}"; do
    sample_name="$(basename "$codestream")"
    log_path="$WORKDIR/run-${tag_trimmed}-${sample_name}.log"

    set +e
    "$BIN_PATH" "$codestream" >"$log_path" 2>&1
    status=$?
    set -e

    if [[ $status -eq 0 ]]; then
      verdict="PASS"
    else
      verdict="FAIL"
    fi

    message="$(cat "$log_path" | tr '\n' ' ' | sed 's/[[:space:]]\\+/ /g' | sed 's/[[:space:]]*$//')"
    printf "%s\t%s\t%s\t%s\n" "$sample_name" "$tag_trimmed" "$verdict" "$message" >> "$RESULTS_FILE"
  done
done

echo "== Result matrix =="
printf "%-14s | %-10s | %-4s | %s\n" "Input" "Tag" "Res" "Message"
printf "%-14s-+-%-10s-+-%-4s-+-%s\n" "--------------" "----------" "----" "----------------------------------------"
while IFS=$'\t' read -r sample tag verdict message; do
  printf "%-14s | %-10s | %-4s | %s\n" "$sample" "$tag" "$verdict" "$message"
done < "$RESULTS_FILE"

echo
echo "Artifacts:"
echo "  matrix: $RESULTS_FILE"
echo "  logs:   $WORKDIR/run-<tag>-<input>.log"
