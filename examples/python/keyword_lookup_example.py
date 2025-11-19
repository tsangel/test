#!/usr/bin/env python3
"""Lookup DICOM keyword -> tag/VR information using dicomsdl."""

from __future__ import annotations

import argparse
import sys

import dicomsdl as dicom


def describe(keyword: str) -> str:
    result = dicom.keyword_to_tag_vr(keyword)
    if result is None:
        return f"{keyword}: not found"
    tag, vr = result
    tag_str = f"({tag.group:04X},{tag.element:04X})"
    return f"{keyword} -> tag={tag_str}, VR={vr}"  # VR.__str__ prints code


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Lookup one or more DICOM keywords and show their tag/VR.",
    )
    parser.add_argument("keywords", nargs="+", help="DICOM keywords (e.g. PatientName)")
    args = parser.parse_args(argv)

    for keyword in args.keywords:
        print(describe(keyword))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
