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


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="dicomconv",
        description="Change DICOM Transfer Syntax UID and write the result.",
        epilog=transfer_syntax_help_epilog(),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("input", help="Input DICOM file path")
    parser.add_argument("output", help="Output DICOM file path")
    parser.add_argument(
        "transfer_syntax",
        help="Transfer syntax keyword or dotted UID value",
    )
    return parser


def resolve_transfer_syntax_uid(text: str) -> dicom.Uid:
    uid = dicom.lookup_uid(text)
    if uid is None:
        raise ValueError(f"Unknown DICOM UID: {text}")
    if uid.type != "Transfer Syntax":
        raise ValueError(f"UID is not a Transfer Syntax UID: {text}")
    return uid


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)

    try:
        transfer_syntax = resolve_transfer_syntax_uid(args.transfer_syntax)
        df = dicom.read_file(args.input)
        df.set_transfer_syntax(transfer_syntax)
        df.write_file(args.output)
        return 0
    except Exception as exc:
        print(f"dicomconv: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
