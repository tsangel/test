#!/usr/bin/env python3
"""Change DICOM Transfer Syntax UID and write to a new file."""

from __future__ import annotations

import argparse
import sys

import dicomsdl as dicom


def transfer_syntax_help_epilog() -> str:
    lines = ["Available Transfer Syntax UIDs (keyword = UID):"]
    for uid in dicom.transfer_syntax_uids():
        keyword = uid.keyword or "-"
        lines.append(f"  {keyword} = {uid.value}")
    return "\n".join(lines)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Change Transfer Syntax UID using dicomsdl Python bindings",
        epilog=transfer_syntax_help_epilog(),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("input", help="Input DICOM file path")
    parser.add_argument("output", help="Output DICOM file path")
    parser.add_argument("transfer_syntax", help="Transfer syntax keyword or dotted UID value")
    return parser.parse_args(argv)


def resolve_transfer_syntax_uid(text: str) -> dicom.Uid:
    uid = dicom.lookup_uid(text)
    if uid is None:
        raise ValueError(f"Unknown DICOM UID: {text}")
    if uid.type != "Transfer Syntax":
        raise ValueError(f"UID is not a Transfer Syntax UID: {text}")
    return uid


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    transfer_syntax = resolve_transfer_syntax_uid(args.transfer_syntax)
    dicom_file = dicom.read_file(args.input)
    dicom_file.set_transfer_syntax(transfer_syntax)
    dicom_file.write_file(args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
