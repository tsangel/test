#!/usr/bin/env python3
"""Lookup DICOM tag metadata via dicomsdl."""

from __future__ import annotations

import argparse
import sys

import dicomsdl as dicom


def parse_tag(text: str) -> int:
    hex_digits = "".join(ch for ch in text if ch not in "() ,")
    if len(hex_digits) != 8:
        raise ValueError
    try:
        return int(hex_digits, 16)
    except ValueError as exc:  # pragma: no cover - same error channel
        raise ValueError from exc


def describe(entry: dict[str, object]) -> str:
    keyword = entry.get("keyword") or "<none>"
    retired = entry.get("retired") or ""
    retired_suffix = " (Retired)" if retired == "R" else ""
    return (
        f"keyword={keyword}, name={entry.get('name')}, VR={entry.get('vr')}"
        f"\n    tag={entry.get('tag')} vm={entry.get('vm')}{retired_suffix}"
    )


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Lookup tag keyword/name from (gggg,eeee) or ggggeeee syntax",
    )
    parser.add_argument("tags", nargs="+", help="Tag strings like (0010,0010) or 00100010")
    args = parser.parse_args(argv)

    for text in args.tags:
        try:
            tag_value = parse_tag(text)
        except ValueError:
            print(f"{text}: invalid tag format")
            continue
        entry = dicom.tag_to_entry(tag_value)
        if entry is None:
            print(f"{text}: not found")
            continue
        print(f"{text} -> {describe(entry)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
