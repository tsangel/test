#!/usr/bin/env python3
"""Lookup well-known DICOM UIDs using the dicomsdl Python bindings."""

from __future__ import annotations

import argparse
import sys

import dicomsdl as dicom


def describe(uid: dicom.Uid, original: str) -> str:
    keyword = uid.keyword
    keyword_str = f", keyword='{keyword}'" if keyword else ""
    return (
        f"{original} -> value='{uid.value}'{keyword_str}, "
        f"name='{uid.name}', type='{uid.type}'"
    )


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Lookup UIDs by dotted value or keyword using dicomsdl",
    )
    parser.add_argument("uids", nargs="+", help="UID dotted values or keywords")
    args = parser.parse_args(argv)

    for text in args.uids:
        uid = dicom.lookup_uid(text)
        if uid is None:
            print(f"{text}: not found")
            continue
        print(describe(uid, text))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
