#!/usr/bin/env bash
#
# This script refreshes all auto-generated DICOM dictionary assets whenever
# a new DICOM Part 06/03 release is published. The steps mirror the manual
# workflow but run in one go for convenience and repeatability.
#
# Usage: misc/dictionary/update_dictionaries.sh
#        (the script figures out the repository root automatically, so it
#         can be invoked from any working directory).

set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DICT_DIR="${ROOT_DIR}/misc/dictionary"
PYTHON_BIN="${PYTHON_BIN:-python3}"

echo "[1/6] Extracting Part 06 tables -> _dataelement_registry.tsv/_uid_registry.tsv/_dicom_version.txt"
"${PYTHON_BIN}" "${DICT_DIR}/extract_part06_tables.py"

echo "[2/6] Regenerating include/dataelement_registry.hpp"
"${PYTHON_BIN}" "${DICT_DIR}/generate_dataelement_registry.py" --source "${DICT_DIR}/_dataelement_registry.tsv" --output "${ROOT_DIR}/include/dataelement_registry.hpp"

echo "[3/6] Regenerating CHD lookup tables (keyword/tag)"
"${PYTHON_BIN}" "${DICT_DIR}/generate_lookup_tables.py" --registry "${DICT_DIR}/_dataelement_registry.tsv" --output "${ROOT_DIR}/include/dataelement_lookup_tables.hpp"

echo "[4/6] Regenerating include/uid_registry.hpp"
"${PYTHON_BIN}" "${DICT_DIR}/generate_uid_registry.py" --source "${DICT_DIR}/_uid_registry.tsv" --output "${ROOT_DIR}/include/uid_registry.hpp"

echo "[5/6] Regenerating UID lookup CHD tables"
"${PYTHON_BIN}" "${DICT_DIR}/generate_uid_lookup_tables.py" --source "${DICT_DIR}/_uid_registry.tsv" --output "${ROOT_DIR}/include/uid_lookup_tables.hpp"

echo "[6/7] Extracting Part 03 Specific Character Sets -> _specific_character_sets.tsv"
"${PYTHON_BIN}" "${DICT_DIR}/extract_part03_specific_character_sets.py"

echo "[7/7] Regenerating include/specific_character_set_registry.hpp"
"${PYTHON_BIN}" "${DICT_DIR}/generate_specific_character_set_registry.py" --source "${DICT_DIR}/_specific_character_sets.tsv" --output "${ROOT_DIR}/include/specific_character_set_registry.hpp"

echo "Done. Reconfigure CMake to propagate the updated version header if needed."
