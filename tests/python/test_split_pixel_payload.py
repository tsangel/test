from __future__ import annotations

import gc
import struct

import dicomsdl as dicom
import pytest


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
    *,
    undefined: bool = False,
) -> bytes:
    header = struct.pack("<HH", group, element) + vr.encode("ascii")
    if undefined:
        return header + b"\x00\x00" + struct.pack("<I", 0xFFFFFFFF)
    if vr in {"OB", "OD", "OF", "OL", "OV", "OW", "SQ", "UC", "UR", "UT", "UN"}:
        return header + b"\x00\x00" + struct.pack("<I", len(value)) + value
    return header + struct.pack("<H", len(value)) + value


def _pack_item(tag_element: int, value: bytes) -> bytes:
    return struct.pack("<HHI", 0xFFFE, tag_element, len(value)) + value


def _build_part10(transfer_syntax_uid: str, body: bytes) -> bytes:
    meta_ts = _pack_explicit_le(0x0002, 0x0010, "UI", _pad_ui(transfer_syntax_uid))
    meta_group_length = _pack_explicit_le(
        0x0002, 0x0000, "UL", struct.pack("<I", len(meta_ts))
    )
    return b"\x00" * 128 + b"DICM" + meta_group_length + meta_ts + body


def _common_pixel_metadata(columns: int, bits_allocated: int, frames: str) -> bytes:
    frames_value = frames.encode("ascii")
    if len(frames_value) % 2 == 1:
        frames_value += b" "
    return b"".join(
        [
            _pack_explicit_le(0x0028, 0x0002, "US", struct.pack("<H", 1)),
            _pack_explicit_le(0x0028, 0x0004, "CS", b"MONOCHROME2 "),
            _pack_explicit_le(0x0028, 0x0008, "IS", frames_value),
            _pack_explicit_le(0x0028, 0x0010, "US", struct.pack("<H", 1)),
            _pack_explicit_le(0x0028, 0x0011, "US", struct.pack("<H", columns)),
            _pack_explicit_le(0x0028, 0x0100, "US", struct.pack("<H", bits_allocated)),
            _pack_explicit_le(0x0028, 0x0101, "US", struct.pack("<H", bits_allocated)),
            _pack_explicit_le(0x0028, 0x0102, "US", struct.pack("<H", bits_allocated - 1)),
            _pack_explicit_le(0x0028, 0x0103, "US", struct.pack("<H", 0)),
        ]
    )


def _build_native_placeholder(placeholder: bytes | None = None) -> bytes:
    if placeholder is None:
        placeholder = dicom.PIXEL_PAYLOAD_PLACEHOLDER_MAGIC
    body = _common_pixel_metadata(columns=3, bits_allocated=16, frames="1")
    body += _pack_explicit_le(0x7FE0, 0x0010, "OB", placeholder)
    return _build_part10("1.2.840.10008.1.2.1", body)


def _build_encap_placeholder(frames: str = "1") -> bytes:
    body = _common_pixel_metadata(columns=2, bits_allocated=8, frames=frames)
    body += _pack_explicit_le(
        0x7FE0, 0x0010, "OB", dicom.PIXEL_PAYLOAD_PLACEHOLDER_MAGIC
    )
    return _build_part10("1.2.840.10008.1.2.1.98", body)


def _single_frame_encap_payload() -> bytes:
    return (
        _pack_item(0xE000, b"")
        + _pack_item(0xE000, b"\x34\x12")
        + _pack_item(0xE0DD, b"")
    )


def test_read_bytes_with_pixel_payload_native_detach_releases_owner() -> None:
    assert dicom.PIXEL_PAYLOAD_PLACEHOLDER_MAGIC == b"DXP1"

    payload = bytearray(b"\x34\x12\x56\x78\x9A\xBC")
    obj = dicom.read_bytes_with_pixel_payload(
        _build_native_placeholder(), payload, name="py-split-native"
    )

    assert obj.path == "py-split-native"
    assert obj.has_attached_pixel_payload is True
    assert hasattr(obj, "_pixel_payload_owner")
    assert obj["PixelData"].vr == dicom.VR.OW
    assert obj.pixel_data(0) == bytes(payload)

    obj.detach_pixel_payload()
    assert obj.has_attached_pixel_payload is False
    assert not hasattr(obj, "_pixel_payload_owner")
    assert obj.Rows == 1
    with pytest.raises(Exception, match="detached"):
        obj.pixel_data(0)


def test_read_bytes_with_pixel_payload_copy_false_keeps_buffers_alive() -> None:
    main_p10 = bytearray(_build_encap_placeholder("1"))
    payload = bytearray(_single_frame_encap_payload())

    obj = dicom.read_bytes_with_pixel_payload(
        main_p10, payload, name="py-split-encap", copy=False
    )
    del main_p10
    del payload
    gc.collect()

    assert obj.has_attached_pixel_payload is True
    assert obj.encoded_pixel_frame_bytes(0) == b"\x34\x12"
    assert obj.pixel_data(0) == b"\x34\x12"


def test_read_bytes_with_pixel_payload_keep_on_error_clears_attached_state() -> None:
    obj = dicom.read_bytes_with_pixel_payload(
        _build_native_placeholder(b"BAD!"),
        b"\x34\x12\x56\x78\x9A\xBC",
        name="py-split-bad-magic",
        keep_on_error=True,
    )

    assert obj.has_error is True
    assert obj.error_message
    assert obj.has_attached_pixel_payload is False
    assert not hasattr(obj, "_pixel_payload_owner")
