from __future__ import annotations

import argparse
import sys

import dicomsdl as dicom


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="dicomshow",
        description="Show a DICOM image with Pillow.",
    )
    parser.add_argument("input", help="Input DICOM file path")
    parser.add_argument(
        "--frame",
        type=int,
        default=0,
        help="Frame index to show (default: 0)",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)

    try:
        dicom.read_file(args.input).to_pil_image(frame=args.frame).show()
        return 0
    except Exception as exc:
        print(f"dicomshow: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
