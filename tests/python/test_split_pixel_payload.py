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


def _build_native_truncated_pixeldata_header() -> bytes:
    body = _common_pixel_metadata(columns=3, bits_allocated=16, frames="1")
    body += struct.pack("<HH", 0x7FE0, 0x0010) + b"OB" + b"\x00\x00" + b"\x04\x00"
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
    marker = obj["PixelData"].value_bytes()
    assert marker.startswith(dicom.PIXEL_PAYLOAD_PLACEHOLDER_MAGIC)
    marker_text = marker[len(dicom.PIXEL_PAYLOAD_PLACEHOLDER_MAGIC) :].decode("utf-8")
    assert "'7fe00010'\tOW" in marker_text
    assert "\\x34\\x12\\x56\\x78" in marker_text
    assert "'7fe00010'\tOW" in obj.dump()
    assert "\\x34\\x12\\x56\\x78" in obj.dump()
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


def test_write_bytes_split_pixel_payload_roundtrip() -> None:
    payload = b"\x34\x12\x56\x78\x9A\xBC"
    native = dicom.read_bytes_with_pixel_payload(
        _build_native_placeholder(), payload, name="py-split-write-native"
    )
    main_bytes, payload_bytes = native.write_bytes_split_pixel_payload()
    assert payload_bytes == payload
    placeholder_only = dicom.read_bytes(main_bytes, name="py-split-write-native-main")
    assert placeholder_only["PixelData"].value_bytes() == dicom.PIXEL_PAYLOAD_PLACEHOLDER_MAGIC
    roundtrip = dicom.read_bytes_with_pixel_payload(
        main_bytes, payload_bytes, name="py-split-write-native-rt"
    )
    assert roundtrip.pixel_data(0) == payload

    encap = dicom.read_bytes_with_pixel_payload(
        _build_encap_placeholder("1"),
        _single_frame_encap_payload(),
        name="py-split-write-encap",
    )
    main_bytes, payload_bytes = encap.write_bytes_split_pixel_payload()
    placeholder_only = dicom.read_bytes(main_bytes, name="py-split-write-encap-main")
    assert placeholder_only["PixelData"].value_bytes() == dicom.PIXEL_PAYLOAD_PLACEHOLDER_MAGIC
    roundtrip = dicom.read_bytes_with_pixel_payload(
        main_bytes, payload_bytes, name="py-split-write-encap-rt"
    )
    assert roundtrip.encoded_pixel_frame_bytes(0) == b"\x34\x12"
    assert roundtrip.pixel_data(0) == b"\x34\x12"


def test_write_with_transfer_syntax_split_pixel_payload_roundtrip() -> None:
    payload = b"\x34\x12\x56\x78\x9A\xBC"
    native = dicom.read_bytes_with_pixel_payload(
        _build_native_placeholder(), payload, name="py-split-write-ts-native"
    )
    main_bytes, payload_bytes = (
        native.write_with_transfer_syntax_split_pixel_payload("ExplicitVRLittleEndian")
    )
    assert payload_bytes == payload
    same_ts_roundtrip = dicom.read_bytes_with_pixel_payload(
        main_bytes, payload_bytes, name="py-split-write-ts-same-rt"
    )
    assert same_ts_roundtrip.pixel_data(0) == payload

    main_bytes, payload_bytes = (
        native.write_with_transfer_syntax_split_pixel_payload("RLELossless")
    )
    placeholder_only = dicom.read_bytes(main_bytes, name="py-split-write-ts-rle-main")
    assert placeholder_only["PixelData"].value_bytes() == dicom.PIXEL_PAYLOAD_PLACEHOLDER_MAGIC
    rle_roundtrip = dicom.read_bytes_with_pixel_payload(
        main_bytes, payload_bytes, name="py-split-write-ts-rle-rt"
    )
    assert rle_roundtrip.pixel_data(0) == payload

    ctx = dicom.create_encoder_context("RLELossless")
    main_bytes, payload_bytes = (
        native.write_with_transfer_syntax_split_pixel_payload(
            "RLELossless", encoder_context=ctx
        )
    )
    rle_roundtrip = dicom.read_bytes_with_pixel_payload(
        main_bytes, payload_bytes, name="py-split-write-ts-rle-ctx-rt"
    )
    assert rle_roundtrip.pixel_data(0) == payload

    main_bytes, payload_bytes = (
        rle_roundtrip.write_with_transfer_syntax_split_pixel_payload(
            "ExplicitVRLittleEndian"
        )
    )
    assert payload_bytes == payload
    native_roundtrip = dicom.read_bytes_with_pixel_payload(
        main_bytes, payload_bytes, name="py-split-write-ts-native-rt"
    )
    assert native_roundtrip.pixel_data(0) == payload


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

    truncated = dicom.read_bytes_with_pixel_payload(
        _build_native_truncated_pixeldata_header(),
        b"\x34\x12\x56\x78\x9A\xBC",
        name="py-split-truncated-main",
        keep_on_error=True,
    )

    assert truncated.has_error is True
    assert truncated.error_message
    assert truncated.has_attached_pixel_payload is False
    assert not hasattr(truncated, "_pixel_payload_owner")
