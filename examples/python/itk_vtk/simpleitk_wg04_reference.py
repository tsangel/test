#!/usr/bin/env python3
"""Reference WG04 single-file reader using SimpleITK."""

from __future__ import annotations

import argparse
import sys

from _wg04_reference_common import add_sample_selection_args, resolve_sample_file


def _load_simpleitk():
    try:
        import SimpleITK as sitk
    except ImportError as exc:
        raise RuntimeError(
            "SimpleITK is not installed. Install it with: pip install SimpleITK"
        ) from exc
    return sitk


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Read a WG04 single-file DICOM image with SimpleITK and print a compact summary.",
    )
    add_sample_selection_args(parser)
    args = parser.parse_args(argv)

    try:
        sitk = _load_simpleitk()
        sample_file, discovered = resolve_sample_file(
            args.path,
            args.sample_index,
            args.sample_name,
            download_missing=args.download_missing,
        )
    except (FileNotFoundError, IndexError, NotADirectoryError, RuntimeError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    image = sitk.ReadImage(str(sample_file))
    array_view = sitk.GetArrayViewFromImage(image)

    print("Framework: SimpleITK")
    print(f"Sample file: {sample_file}")
    print(f"Discovered sample files: {len(discovered)}")
    print(f"Image size (x, y, z): {tuple(image.GetSize())}")
    print(f"Array shape: {tuple(array_view.shape)}")
    print(f"Pixel type: {image.GetPixelIDTypeAsString()}")
    print(f"Spacing: {tuple(image.GetSpacing())}")
    print(f"Origin: {tuple(image.GetOrigin())}")
    print(f"Direction: {tuple(image.GetDirection())}")
    print(f"Value range: min={array_view.min()} max={array_view.max()}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
