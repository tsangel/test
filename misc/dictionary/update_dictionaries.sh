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
cd "${ROOT_DIR}"

REPORT_FILES=(
    "misc/dictionary/_dataelement_registry.tsv"
    "misc/dictionary/_uid_registry.tsv"
    "misc/dictionary/_specific_character_sets.tsv"
)

get_head_version() {
    git -C "${ROOT_DIR}" show "HEAD:misc/dictionary/_dicom_version.txt" 2>/dev/null | tr -d '\r' | head -n1 || true
}

get_worktree_version() {
    if [[ -f "${DICT_DIR}/_dicom_version.txt" ]]; then
        tr -d '\r' < "${DICT_DIR}/_dicom_version.txt" | head -n1
    else
        echo ""
    fi
}

sanitize_version_for_filename() {
    local raw="$1"
    local sanitized
    if [[ -z "${raw}" ]]; then
        echo "unknown"
        return
    fi
    sanitized="$(printf '%s' "${raw}" | tr -cs 'A-Za-z0-9._-' '_')"
    sanitized="${sanitized##_}"
    sanitized="${sanitized%%_}"
    if [[ -z "${sanitized}" ]]; then
        echo "unknown"
    else
        echo "${sanitized}"
    fi
}

file_change_status() {
    local rel_path="$1"
    if git -C "${ROOT_DIR}" diff --quiet HEAD -- "${rel_path}"; then
        echo "no changes"
    else
        echo "changed"
    fi
}

print_key_tsv_change_status() {
    local uid_status
    local charset_status
    uid_status="$(file_change_status "misc/dictionary/_uid_registry.tsv")"
    charset_status="$(file_change_status "misc/dictionary/_specific_character_sets.tsv")"
    echo "Key TSV change status (vs HEAD):"
    echo "- _uid_registry.tsv: ${uid_status}"
    echo "- _specific_character_sets.tsv: ${charset_status}"
}

sync_dicom_standard_version_define() {
    local new_version="$1"
    local const_path="include/dicom_const.h"
    local tmp_path

    if [[ -z "${new_version}" ]]; then
        echo "Skipping dicom_const.h sync: empty DICOM version."
        return
    fi
    if [[ ! -f "${const_path}" ]]; then
        echo "Skipping dicom_const.h sync: ${const_path} not found."
        return
    fi

    tmp_path="$(mktemp "${const_path}.tmp.XXXXXX")"
    awk -v version="${new_version}" '
        BEGIN { updated = 0 }
        /^#define DICOM_STANDARD_VERSION "/ {
            print "#define DICOM_STANDARD_VERSION \"" version "\""
            updated = 1
            next
        }
        { print }
        END {
            if (!updated) {
                exit 42
            }
        }
    ' "${const_path}" > "${tmp_path}" || {
        rm -f "${tmp_path}"
        echo "Failed to update ${const_path}: DICOM_STANDARD_VERSION define not found."
        exit 1
    }
    mv "${tmp_path}" "${const_path}"
    echo "Synced ${const_path} DICOM_STANDARD_VERSION -> ${new_version}"
}

write_version_diff_report() {
    local old_version="$1"
    local new_version="$2"
    local safe_old
    local safe_new
    local report_path
    local changed_files=()
    local rel_path

    safe_old="$(sanitize_version_for_filename "${old_version}")"
    safe_new="$(sanitize_version_for_filename "${new_version}")"
    report_path="${DICT_DIR}/dictionary_diff_${safe_old}_to_${safe_new}.md"

    for rel_path in "${REPORT_FILES[@]}"; do
        if ! git -C "${ROOT_DIR}" diff --quiet HEAD -- "${rel_path}"; then
            changed_files+=("${rel_path}")
        fi
    done

    {
        echo "# DICOM Dictionary Update Diff"
        echo ""
        echo "- Version: ${old_version} -> ${new_version}"
        echo "- Scope: TSV outputs only"
        echo ""
        echo "## Key TSV Change Status"
        echo ""
        echo "- _uid_registry.tsv: $(file_change_status "misc/dictionary/_uid_registry.tsv")"
        echo "- _specific_character_sets.tsv: $(file_change_status "misc/dictionary/_specific_character_sets.tsv")"
        echo ""
        echo "## Diffstat"
        echo ""
        echo '```diff'
        if [[ "${#changed_files[@]}" -eq 0 ]]; then
            echo "No changed dictionary output files."
        else
            for rel_path in "${changed_files[@]}"; do
                git -C "${ROOT_DIR}" --no-pager diff --no-color --stat HEAD -- "${rel_path}" || true
            done
        fi
        echo '```'
        echo ""
        echo "## File Diffs"
        echo ""
    } > "${report_path}"

    if [[ "${#changed_files[@]}" -eq 0 ]]; then
        {
            echo "No diff detected against HEAD for dictionary output files."
            echo ""
        } >> "${report_path}"
    else
        for rel_path in "${changed_files[@]}"; do
            {
                echo "### ${rel_path}"
                echo ""
                echo '```diff'
                git -C "${ROOT_DIR}" --no-pager diff --no-color HEAD -- "${rel_path}" || true
                echo '```'
                echo ""
            } >> "${report_path}"
        done
    fi

    echo "Version change detected (${old_version} -> ${new_version})."
    echo "Wrote consolidated diff report: ${report_path}"
}

HEAD_DICOM_VERSION="$(get_head_version)"

echo "[1/8] Extracting Part 06 tables -> _dataelement_registry.tsv/_uid_registry.tsv/_dicom_version.txt"
"${PYTHON_BIN}" "${DICT_DIR}/extract_part06_tables.py"

echo "[2/8] Regenerating include/dataelement_registry.hpp"
"${PYTHON_BIN}" "${DICT_DIR}/generate_dataelement_registry.py" --source "misc/dictionary/_dataelement_registry.tsv" --output "include/dataelement_registry.hpp"

echo "[3/8] Regenerating CHD lookup tables (keyword/tag)"
"${PYTHON_BIN}" "${DICT_DIR}/generate_lookup_tables.py" --registry "misc/dictionary/_dataelement_registry.tsv" --output "include/dataelement_lookup_tables.hpp"

echo "[4/8] Regenerating include/uid_registry.hpp"
"${PYTHON_BIN}" "${DICT_DIR}/generate_uid_registry.py" --source "misc/dictionary/_uid_registry.tsv" --output "include/uid_registry.hpp"

echo "[5/8] Regenerating UID lookup CHD tables"
"${PYTHON_BIN}" "${DICT_DIR}/generate_uid_lookup_tables.py" --source "misc/dictionary/_uid_registry.tsv" --output "include/uid_lookup_tables.hpp"

echo "[6/8] Extracting Part 03 Specific Character Sets -> _specific_character_sets.tsv"
"${PYTHON_BIN}" "${DICT_DIR}/extract_part03_specific_character_sets.py"

echo "[7/8] Regenerating include/specific_character_set_registry.hpp"
"${PYTHON_BIN}" "${DICT_DIR}/generate_specific_character_set_registry.py" --source "misc/dictionary/_specific_character_sets.tsv" --output "include/specific_character_set_registry.hpp"

echo "[8/8] Syncing include/dicom_const.h DICOM_STANDARD_VERSION"
sync_dicom_standard_version_define "$(get_worktree_version)"

print_key_tsv_change_status

WORKTREE_DICOM_VERSION="$(get_worktree_version)"
if [[ -n "${HEAD_DICOM_VERSION}" && -n "${WORKTREE_DICOM_VERSION}" && "${HEAD_DICOM_VERSION}" != "${WORKTREE_DICOM_VERSION}" ]]; then
    write_version_diff_report "${HEAD_DICOM_VERSION}" "${WORKTREE_DICOM_VERSION}"
else
    echo "No DICOM version change detected (_dicom_version.txt). Skipping diff report."
fi

echo "Done. CMake will regenerate temporary VERSION cache files from include/dicom_const.h."
