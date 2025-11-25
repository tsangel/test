#!/usr/bin/env bash
# Build dicomsdl documentation (Doxygen + Sphinx HTML).
# Usage: ./build_docs.sh

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DOCS_DIR="$ROOT/docs"
BUILD_DIR="$DOCS_DIR/_build"
DOXY_DIR="$BUILD_DIR/doxygen"
HTML_DIR="$BUILD_DIR/html"

mkdir -p "$DOXY_DIR" "$HTML_DIR"

echo "==> Running Doxygen..."
doxygen "$ROOT/Doxyfile"

echo "==> Building Sphinx HTML..."
sphinx-build -b html "$DOCS_DIR" "$HTML_DIR"

echo "Docs built at: $HTML_DIR"
