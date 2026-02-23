#!/usr/bin/env python3
"""Show raw DataElement bytes via value_span()."""

from __future__ import annotations

import argparse
import sys

import dicomsdl as dicom


def preview(view: memoryview, count: int) -> list[int]:
    return list(view[:count])


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Print raw value-byte previews for a DICOM element",
    )
    parser.add_argument("path", help="Path to a DICOM file")
    parser.add_argument(
        "tag",
        nargs="?",
        default="PixelData",
        help="Keyword/tag/tag-path (default: PixelData)",
    )
    parser.add_argument(
        "--preview",
        type=int,
        default=16,
        help="Number of bytes to print (default: 16)",
    )
    args = parser.parse_args(argv)

    dataset = dicom.read_file(args.path).dataset
    element = dataset.get_dataelement(args.tag)
    if not element:
        print(f"{args.tag}: not found")
        return 1

    raw = element.value_span()

    print(f"tag={element.tag} vr={element.vr} length={element.length} vm={element.vm}")
    print(f"value_span: nbytes={raw.nbytes} preview={preview(raw, args.preview)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
