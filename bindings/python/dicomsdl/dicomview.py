from __future__ import annotations

import argparse
from pathlib import Path
import sys


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="dicomview",
        description="Browse DICOM files in a folder and preview the selected image with Qt.",
    )
    parser.add_argument(
        "input",
        nargs="?",
        help="Input DICOM file or folder path (default: last saved path or current directory)",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)

    if sys.version_info < (3, 10):
        print("dicomview requires Python 3.10 or newer.", file=sys.stderr)
        return 1

    try:
        from ._viewer_app import run_viewer
    except ImportError as exc:
        print(f"dicomview: {exc}", file=sys.stderr)
        return 1

    try:
        target = Path(args.input).expanduser() if args.input else None
        return int(run_viewer(target))
    except Exception as exc:
        print(f"dicomview: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
