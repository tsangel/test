#!/usr/bin/env python3
"""Extract version strings from include/dicom_const.h."""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

_DICOMSDL_VERSION_PATTERN = re.compile(
    r'^\s*#define\s+DICOMSDL_VERSION\s+"([^"]+)"\s*$'
)
_DICOM_STANDARD_VERSION_PATTERN = re.compile(
    r'^\s*#define\s+DICOM_STANDARD_VERSION\s+"([^"]+)"\s*$'
)


def _parse_versions(header_path: pathlib.Path) -> tuple[str, str]:
    text = header_path.read_text(encoding="utf-8")
    dicomsdl_version = ""
    dicom_standard_version = ""
    for line in text.splitlines():
        if not dicomsdl_version:
            match = _DICOMSDL_VERSION_PATTERN.match(line)
            if match:
                dicomsdl_version = match.group(1).strip()
                continue
        if not dicom_standard_version:
            match = _DICOM_STANDARD_VERSION_PATTERN.match(line)
            if match:
                dicom_standard_version = match.group(1).strip()
                continue

    if not dicomsdl_version:
        raise RuntimeError(
            f"Could not find DICOMSDL_VERSION macro in header: {header_path}"
        )
    if not dicom_standard_version:
        raise RuntimeError(
            f"Could not find DICOM_STANDARD_VERSION macro in header: {header_path}"
        )
    return dicomsdl_version, dicom_standard_version


def _write_value(path: pathlib.Path, value: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(f"{value}\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--header", required=True, help="Path to include/dicom_const.h")
    parser.add_argument("--version-out", required=True, help="Output VERSION file path")
    parser.add_argument(
        "--dicom-standard-out",
        required=True,
        help="Output DICOM standard version file path",
    )
    args = parser.parse_args()

    header_path = pathlib.Path(args.header).resolve()
    version_out = pathlib.Path(args.version_out).resolve()
    dicom_standard_out = pathlib.Path(args.dicom_standard_out).resolve()

    dicomsdl_version, dicom_standard_version = _parse_versions(header_path)
    _write_value(version_out, dicomsdl_version)
    _write_value(dicom_standard_out, dicom_standard_version)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:  # pragma: no cover - command-line error path
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(1)
