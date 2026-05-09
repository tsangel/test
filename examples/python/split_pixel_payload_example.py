#!/usr/bin/env python3
"""Read a split PixelData payload with DicomSDL.

Usage:
    python split_pixel_payload_example.py
    python split_pixel_payload_example.py main-p10.dcm pixel-payload.bin --frame 0

With no input paths, this runs a tiny built-in native PixelData demo. For a real
encapsulated payload, pixel-payload.bin must contain the full PixelData value
field: Basic Offset Table item, fragments, and sequence delimiter.
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path
from typing import Sequence

import dicomsdl as dicom


def _pad_ui(text: str) -> bytes:
    raw = text.encode("ascii")
    if len(raw) % 2 == 1:
        raw += b"\x00"
    return raw


def _pack_explicit_le(
    group: int,
    element: int,
    vr: str,
    value: bytes,
) -> bytes:
    header = struct.pack("<HH", group, element) + vr.encode("ascii")
    if vr in {"OB", "OD", "OF", "OL", "OV", "OW", "SQ", "UC", "UR", "UT", "UN"}:
        return header + b"\x00\x00" + struct.pack("<I", len(value)) + value
    return header + struct.pack("<H", len(value)) + value


def _build_part10(transfer_syntax_uid: str, body: bytes) -> bytes:
    meta_ts = _pack_explicit_le(0x0002, 0x0010, "UI", _pad_ui(transfer_syntax_uid))
    meta_length = _pack_explicit_le(0x0002, 0x0000, "UL", struct.pack("<I", len(meta_ts)))
    return b"\x00" * 128 + b"DICM" + meta_length + meta_ts + body


def _build_demo_main_p10() -> bytes:
    body = b"".join(
        [
            _pack_explicit_le(0x0028, 0x0002, "US", struct.pack("<H", 1)),
            _pack_explicit_le(0x0028, 0x0004, "CS", b"MONOCHROME2 "),
            _pack_explicit_le(0x0028, 0x0008, "IS", b"1 "),
            _pack_explicit_le(0x0028, 0x0010, "US", struct.pack("<H", 1)),
            _pack_explicit_le(0x0028, 0x0011, "US", struct.pack("<H", 3)),
            _pack_explicit_le(0x0028, 0x0100, "US", struct.pack("<H", 16)),
            _pack_explicit_le(0x0028, 0x0101, "US", struct.pack("<H", 16)),
            _pack_explicit_le(0x0028, 0x0102, "US", struct.pack("<H", 15)),
            _pack_explicit_le(0x0028, 0x0103, "US", struct.pack("<H", 0)),
            _pack_explicit_le(
                0x7FE0, 0x0010, "OB", dicom.PIXEL_PAYLOAD_PLACEHOLDER_MAGIC
            ),
        ]
    )
    return _build_part10("1.2.840.10008.1.2.1", body)


def _hex_prefix(data: bytes, max_count: int = 16) -> str:
    prefix = " ".join(f"{byte:02x}" for byte in data[:max_count])
    return prefix + (" ..." if len(data) > max_count else "")


def _run(name: str, main_p10: bytes | bytearray, payload: bytes | bytearray, frame: int) -> int:
    dicom_file = dicom.read_bytes_with_pixel_payload(
        main_p10, payload, name=name, copy=False
    )

    print(f"Loaded split DICOM: {dicom_file.path}")
    print(f"TransferSyntaxUID: {dicom_file.transfer_syntax_uid.value}")
    print(f"Rows x Columns: {dicom_file.Rows} x {dicom_file.Columns}")
    print(f"PixelData VR after attach: {dicom_file['PixelData'].vr.str()}")
    print(f"Attached payload: {dicom_file.has_attached_pixel_payload}")

    decoded = dicom_file.pixel_data(frame)
    suffix = f" [{_hex_prefix(decoded)}]" if decoded else ""
    print(f"Decoded frame {frame}: {len(decoded)} bytes{suffix}")

    dicom_file.detach_pixel_payload()
    payload.clear()

    print(f"Attached payload after detach: {dicom_file.has_attached_pixel_payload}")
    print(f"Rows still available after detach: {dicom_file.Rows}")
    print("After detach, do not call pixel decode APIs until a payload is attached again.")

    return 0


def main(argv: Sequence[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Read a DXP1 split PixelData payload and detach it after decode",
    )
    parser.add_argument("main_p10", nargs="?", help="Main P10 file containing DXP1 PixelData")
    parser.add_argument("pixel_payload", nargs="?", help="External PixelData value bytes")
    parser.add_argument("--frame", type=int, default=0, help="Frame index to decode")
    args = parser.parse_args(argv)

    if args.main_p10 is None and args.pixel_payload is None:
        return _run(
            "built-in-split-pixel-payload-demo",
            bytearray(_build_demo_main_p10()),
            bytearray(b"\x34\x12\x56\x78\x9A\xBC"),
            args.frame,
        )

    if args.main_p10 is None or args.pixel_payload is None:
        parser.error("provide both main_p10 and pixel_payload, or neither for the demo")

    return _run(
        args.main_p10,
        bytearray(Path(args.main_p10).read_bytes()),
        bytearray(Path(args.pixel_payload).read_bytes()),
        args.frame,
    )


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
