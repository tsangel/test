#!/usr/bin/env python3
"""Decode pixel data with a safe metadata-refresh pattern.

Warning:
If transfer syntax or pixel-affecting tags change, do not reuse old decode
layout assumptions or output buffers. Re-load/re-query before decoding again.
"""

from __future__ import annotations

import argparse
import sys
from typing import List

import numpy as np

import dicomsdl as dicom


def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Decode one frame safely and show shape/dtype",
    )
    parser.add_argument("path", help="Path to input DICOM file")
    parser.add_argument("--frame", type=int, default=0, help="Frame index (default: 0)")
    parser.add_argument(
        "--scaled",
        action="store_true",
        help="Enable scaled output (Modality LUT/Rescale for monochrome when available)",
    )
    args = parser.parse_args(argv)

    dicom_file = dicom.read_file(args.path)
    arr = dicom_file.to_array(frame=args.frame, scaled=args.scaled)
    print(f"decoded: shape={arr.shape}, dtype={arr.dtype}")

    # Safe buffer reuse pattern:
    # Allocate output buffer from the latest decoded layout, then decode_into.
    out = np.empty_like(arr)
    dicom_file.decode_into(out, frame=args.frame, scaled=args.scaled)
    print(f"decode_into: shape={out.shape}, dtype={out.dtype}")

    # If pixel-related metadata changes, refresh before decoding again.
    # Example policy:
    #   dicom_file = dicom.read_file(args.path)
    #   arr = dicom_file.to_array(frame=args.frame, scaled=args.scaled)
    #   out = np.empty_like(arr)

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
