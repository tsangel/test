#!/usr/bin/env bash
# Build dicomsdl documentation (Doxygen + Sphinx HTML).
# Usage: ./build-docs.sh [html|html-all|check]

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DOCS_DIR="$ROOT/docs"
BUILD_DIR="$DOCS_DIR/_build"
DOXY_DIR="$BUILD_DIR/doxygen"
HTML_DIR="$BUILD_DIR/html"
MULTILANG_DIR="$BUILD_DIR/html-multilang"
DOC_LANGUAGE="${DICOMSDL_DOC_LANGUAGE:-en}"
TARGET="${1:-html}"
SUPPORTED_LANGUAGES=(en ko ja zh-cn)
TRANSLATION_LANGUAGES=(ko ja zh-cn)

mkdir -p "$DOXY_DIR" "$HTML_DIR" "$MULTILANG_DIR"

usage() {
  cat <<'EOF'
Usage: ./build-docs.sh [html|html-all|check]

Targets:
  html            Build one HTML site using DICOMSDL_DOC_LANGUAGE (default: en)
  html-all        Build HTML for en, ko, ja, and zh-cn
  check           Verify that ko / ja / zh-cn mirror docs/en paths
EOF
}

normalize_lang() {
  local lang="${1:-en}"
  case "$lang" in
    zh_CN|zh-CN|zh-Hans)
      printf '%s\n' "zh-cn"
      ;;
    *)
      printf '%s\n' "$lang"
      ;;
  esac
}

run_doxygen() {
  echo "==> Running Doxygen..."
  doxygen "$ROOT/Doxyfile"
}

check_doc_trees() {
  echo "==> Checking translation tree consistency..."
  python3 "$ROOT/scripts/check_doc_translations.py" "$DOCS_DIR"
}

active_doc_languages_for_single_build() {
  local lang="$1"
  if [[ "$lang" == "en" ]]; then
    printf '%s\n' "en"
  else
    printf '%s\n' "en,$lang"
  fi
}

active_doc_languages_for_html_all() {
  local joined="${SUPPORTED_LANGUAGES[*]}"
  printf '%s\n' "${joined// /,}"
}

build_html() {
  local lang="$1"
  local source_dir="$DOCS_DIR/$lang"
  local out_dir="$2"
  local active_languages="$3"
  echo "==> Building Sphinx HTML for language=$lang ..."
  DICOMSDL_DOC_LANGUAGE="$lang" \
  DICOMSDL_ACTIVE_DOC_LANGUAGES="$active_languages" \
  sphinx-build -c "$DOCS_DIR" -b html "$source_dir" "$out_dir"
  echo "Docs built at: $out_dir"
}

case "$TARGET" in
  html)
    check_doc_trees
    run_doxygen
    DOC_LANGUAGE="$(normalize_lang "$DOC_LANGUAGE")"
    ACTIVE_LANGUAGES="$(active_doc_languages_for_single_build "$DOC_LANGUAGE")"
    if [[ "$DOC_LANGUAGE" == "en" ]]; then
      build_html "$DOC_LANGUAGE" "$HTML_DIR" "$ACTIVE_LANGUAGES"
    else
      build_html "$DOC_LANGUAGE" "$HTML_DIR/$DOC_LANGUAGE" "$ACTIVE_LANGUAGES"
    fi
    ;;
  html-all)
    check_doc_trees
    run_doxygen
    ACTIVE_LANGUAGES="$(active_doc_languages_for_html_all)"
    build_html "en" "$MULTILANG_DIR/en" "$ACTIVE_LANGUAGES"
    for lang in "${TRANSLATION_LANGUAGES[@]}"; do
      build_html "$lang" "$MULTILANG_DIR/$lang" "$ACTIVE_LANGUAGES"
    done
    ;;
  check)
    check_doc_trees
    ;;
  -h|--help|help)
    usage
    ;;
  *)
    usage >&2
    exit 1
    ;;
esac
