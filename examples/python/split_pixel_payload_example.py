#!/usr/bin/env python3
"""Read or create a split PixelData payload with DicomSDL.

Usage:
    python split_pixel_payload_example.py
    python split_pixel_payload_example.py main-p10.dcm pixel-payload.bin --frame 0
    python split_pixel_payload_example.py --split source.dcm main-out.dcm pixel-payload.bin
    python split_pixel_payload_example.py --split source.dcm main-out.dcm pixel-payload.bin \
        --transfer-syntax RLELossless

The split-payload convention is private to DicomSDL runtime workflows:

* The main P10 DICOM stores (7FE0,0010) PixelData as the fixed 4-byte DXP1
  placeholder.
* The sidecar payload stores the complete PixelData value bytes.
* For encapsulated transfer syntaxes, the sidecar starts with the Basic Offset
  Table item and includes all fragment items plus the sequence delimiter.

Use write_bytes_split_pixel_payload() to split without transcoding, or
write_with_transfer_syntax_split_pixel_payload() to serialize through a target
transfer syntax while splitting. After a viewer decodes the frames it needs,
call detach_pixel_payload() before releasing the caller-owned payload memory.
Pass keep_dump=True if you want the detached marker to retain PixelData dump
text for diagnostics.
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


def _print_decode_summary(dicom_file: dicom.DicomFile, frame: int) -> None:
    pixel_data = dicom_file["PixelData"]
    if pixel_data.vr == dicom.VR.PX:
        try:
            encoded = dicom_file.encoded_pixel_frame_bytes(frame)
            suffix = f" [{_hex_prefix(encoded)}]" if encoded else ""
            print(f"Encoded frame {frame}: {len(encoded)} bytes{suffix}")
        except Exception as exc:
            print(f"Encoded frame {frame}: unavailable ({exc})")

    try:
        decoded = dicom_file.pixel_data(frame)
        suffix = f" [{_hex_prefix(decoded)}]" if decoded else ""
        print(f"Decoded frame {frame}: {len(decoded)} bytes{suffix}")
    except Exception as exc:
        print(f"Decoded frame {frame}: unavailable ({exc})")


def _print_attached_summary(dicom_file: dicom.DicomFile, frame: int) -> None:
    print(f"TransferSyntaxUID: {dicom_file.transfer_syntax_uid.value}")
    print(f"Rows x Columns: {dicom_file.Rows} x {dicom_file.Columns}")
    print(f"PixelData VR after attach: {dicom_file['PixelData'].vr.str()}")
    print(f"Attached payload: {dicom_file.has_attached_pixel_payload}")
    _print_decode_summary(dicom_file, frame)


def _run_read_split(
    name: str,
    main_p10: bytes | bytearray,
    payload: bytearray,
    frame: int,
    keep_dump: bool,
) -> int:
    dicom_file = dicom.read_bytes_with_pixel_payload(
        main_p10, payload, name=name, copy=False
    )

    print(f"Loaded split DICOM: {dicom_file.path}")
    _print_attached_summary(dicom_file, frame)

    dicom_file.detach_pixel_payload(keep_dump=keep_dump)
    payload.clear()

    print(f"Attached payload after detach: {dicom_file.has_attached_pixel_payload}")
    print(f"Rows still available after detach: {dicom_file.Rows}")
    if keep_dump:
        print("Detached marker keeps PixelData dump text for diagnostics.")
    print("After detach, do not call pixel decode APIs until a payload is attached again.")

    return 0


def _run_split_source(
    source_path: Path,
    main_out_path: Path,
    payload_out_path: Path,
    transfer_syntax: str | None,
    frame: int,
    keep_dump: bool,
) -> int:
    source = dicom.read_file(source_path)

    if transfer_syntax:
        main_bytes, payload_bytes = source.write_with_transfer_syntax_split_pixel_payload(
            transfer_syntax
        )
        print(f"Split source through target transfer syntax: {transfer_syntax}")
    else:
        main_bytes, payload_bytes = source.write_bytes_split_pixel_payload()
        print("Split source with its current transfer syntax.")

    main_out_path.write_bytes(main_bytes)
    payload_out_path.write_bytes(payload_bytes)
    print(f"Wrote main DICOM: {main_out_path} ({len(main_bytes)} bytes)")
    print(f"Wrote PixelData payload: {payload_out_path} ({len(payload_bytes)} bytes)")

    placeholder_only = dicom.read_bytes(main_bytes, name="split-main-placeholder-check")
    if placeholder_only["PixelData"].value_bytes() != dicom.PIXEL_PAYLOAD_PLACEHOLDER_MAGIC:
        raise RuntimeError("split main DICOM does not contain the DXP1 PixelData placeholder")
    print("Verified main DICOM has the DXP1 PixelData placeholder.")

    main_owner = bytearray(main_bytes)
    payload_owner = bytearray(payload_bytes)
    rejoined = dicom.read_bytes_with_pixel_payload(
        main_owner,
        payload_owner,
        name="split-roundtrip-check",
        copy=False,
    )
    print("Reattached split payload for a roundtrip check.")
    _print_attached_summary(rejoined, frame)

    rejoined.detach_pixel_payload(keep_dump=keep_dump)
    payload_owner.clear()
    print("Detached roundtrip payload; metadata remains available.")
    return 0


def main(argv: Sequence[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Read or create a DXP1 split PixelData payload",
        epilog=(
            "Without --split, provide main_p10 and pixel_payload to read an already "
            "split instance. With --split, provide source.dcm, main-out.dcm, and "
            "pixel-payload.bin to create split outputs. The optional "
            "--transfer-syntax value can be a UID or DicomSDL keyword such as "
            "ExplicitVRLittleEndian, ExplicitVRBigEndian, or RLELossless."
        ),
    )
    parser.add_argument(
        "--split",
        action="store_true",
        help="split a normal source DICOM into main DICOM and PixelData payload files",
    )
    parser.add_argument(
        "--transfer-syntax",
        help="target transfer syntax to use while splitting",
    )
    parser.add_argument(
        "--keep-dump",
        action="store_true",
        help="keep PixelData dump text in the detached marker",
    )
    parser.add_argument("--frame", type=int, default=0, help="Frame index to decode")
    parser.add_argument("paths", nargs="*", help="input/output paths; see usage examples")
    args = parser.parse_args(argv)

    if args.split:
        if len(args.paths) != 3:
            parser.error("--split requires: source.dcm main-out.dcm pixel-payload.bin")
        return _run_split_source(
            Path(args.paths[0]),
            Path(args.paths[1]),
            Path(args.paths[2]),
            args.transfer_syntax,
            args.frame,
            args.keep_dump,
        )

    if args.transfer_syntax:
        parser.error("--transfer-syntax is only valid with --split")

    if not args.paths:
        return _run_read_split(
            "built-in-split-pixel-payload-demo",
            bytearray(_build_demo_main_p10()),
            bytearray(b"\x34\x12\x56\x78\x9A\xBC"),
            args.frame,
            args.keep_dump,
        )

    if len(args.paths) != 2:
        parser.error("provide main_p10 and pixel_payload, or use --split")

    return _run_read_split(
        args.paths[0],
        bytearray(Path(args.paths[0]).read_bytes()),
        bytearray(Path(args.paths[1]).read_bytes()),
        args.frame,
        args.keep_dump,
    )


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
