from __future__ import annotations

import gc
import struct

import dicomsdl as dicom
import numpy as np
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
        return header + b"\x00\x00" + struct.pack("<I", 0xFFFFFFFF) + value
    if vr in {"OB", "OD", "OF", "OL", "OV", "OW", "SQ", "UC", "UR", "UT", "UN"}:
        return header + b"\x00\x00" + struct.pack("<I", len(value)) + value
    return header + struct.pack("<H", len(value)) + value


def _pack_item(tag_element: int, value: bytes) -> bytes:
    return struct.pack("<HHI", 0xFFFE, tag_element, len(value)) + value


def _pack_implicit_le(group: int, element: int, value: bytes) -> bytes:
    return struct.pack("<HHI", group, element, len(value)) + value


def _placeholder_value(vr: str, vl: int, payload_length: int) -> bytes:
    return (
        dicom.PIXELDATA_PAYLOAD_PLACEHOLDER_MAGIC
        + vr.encode("ascii")
        + struct.pack("<IQ", vl, payload_length)
    )


def _is_placeholder_metadata(value: bytes) -> bool:
    return value.startswith(dicom.PIXELDATA_PAYLOAD_PLACEHOLDER_MAGIC) and len(value) == 22


def _clone_descriptor(desc: dicom.PixelPayloadDecodeDescriptor) -> dicom.PixelPayloadDecodeDescriptor:
    out = dicom.PixelPayloadDecodeDescriptor()
    out.transfer_syntax_uid = desc.transfer_syntax_uid
    out.photometric = desc.photometric
    out.rows = desc.rows
    out.cols = desc.cols
    out.frames = desc.frames
    out.samples_per_pixel = desc.samples_per_pixel
    out.bits_allocated = desc.bits_allocated
    out.bits_stored = desc.bits_stored
    out.pixel_representation = desc.pixel_representation
    out.planar_configuration = desc.planar_configuration
    out.expected_payload_length = desc.expected_payload_length
    out.frame_fragments = desc.frame_fragments
    out.source_name = desc.source_name
    return out


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
        placeholder = _placeholder_value("OW", 6, 6)
    body = _common_pixel_metadata(columns=3, bits_allocated=16, frames="1")
    body += _pack_explicit_le(0x7FE0, 0x0010, "OB", placeholder)
    return _build_part10("1.2.840.10008.1.2.1", body)


def _build_native_full(payload: bytes) -> bytes:
    body = _common_pixel_metadata(columns=3, bits_allocated=16, frames="1")
    body += _pack_explicit_le(0x7FE0, 0x0010, "OW", payload)
    return _build_part10("1.2.840.10008.1.2.1", body)


def _build_native_implicit_full(payload: bytes) -> bytes:
    body = b"".join(
        [
            _pack_implicit_le(0x0028, 0x0002, struct.pack("<H", 1)),
            _pack_implicit_le(0x0028, 0x0004, b"MONOCHROME2 "),
            _pack_implicit_le(0x0028, 0x0008, b"1 "),
            _pack_implicit_le(0x0028, 0x0010, struct.pack("<H", 1)),
            _pack_implicit_le(0x0028, 0x0011, struct.pack("<H", 3)),
            _pack_implicit_le(0x0028, 0x0100, struct.pack("<H", 16)),
            _pack_implicit_le(0x0028, 0x0101, struct.pack("<H", 16)),
            _pack_implicit_le(0x0028, 0x0102, struct.pack("<H", 15)),
            _pack_implicit_le(0x0028, 0x0103, struct.pack("<H", 0)),
            _pack_implicit_le(0x7FE0, 0x0010, payload),
        ]
    )
    return _build_part10("1.2.840.10008.1.2", body)


def _build_native_truncated_pixeldata_header() -> bytes:
    body = _common_pixel_metadata(columns=3, bits_allocated=16, frames="1")
    body += struct.pack("<HH", 0x7FE0, 0x0010) + b"OB" + b"\x00\x00" + b"\x04\x00"
    return _build_part10("1.2.840.10008.1.2.1", body)


def _build_encap_placeholder(frames: str = "1", payload_length: int | None = None) -> bytes:
    if payload_length is None:
        payload_length = len(_single_frame_encap_payload())
    body = _common_pixel_metadata(columns=2, bits_allocated=8, frames=frames)
    body += _pack_explicit_le(
        0x7FE0, 0x0010, "OB", _placeholder_value("OB", 0xFFFFFFFF, payload_length)
    )
    return _build_part10("1.2.840.10008.1.2.1.98", body)


def _single_frame_encap_payload() -> bytes:
    return (
        _pack_item(0xE000, b"")
        + _pack_item(0xE000, b"\x34\x12")
        + _pack_item(0xE0DD, b"")
    )


def _two_frame_encap_payload() -> bytes:
    return (
        _pack_item(0xE000, struct.pack("<II", 0, 10))
        + _pack_item(0xE000, b"\x01\x02")
        + _pack_item(0xE000, b"\x03\x04")
        + _pack_item(0xE0DD, b"")
    )


def test_split_pixeldata_payload_roundtrip_native() -> None:
    payload = b"\x34\x12\x56\x78\x9A\xBC"
    source = _build_native_full(payload)
    raw_pixel_element = _pack_explicit_le(0x7FE0, 0x0010, "OW", payload)

    split = dicom.split_pixeldata_payload([], source)
    assert split.ok is True
    main = split.main_bytes
    split_payload = split.pixel_payload
    assert split_payload == payload
    desc = split.decode_descriptor
    assert desc.expected_payload_length == len(payload)

    placeholder = _pack_explicit_le(
        0x7FE0, 0x0010, "OB", _placeholder_value("OW", len(payload), len(payload))
    )
    assert main == source.replace(raw_pixel_element, placeholder, 1)
    assert dicom.join_pixeldata_payload(main, split_payload) == source

    rejoined = dicom.read_bytes_with_pixeldata_payload(main, split_payload)
    assert rejoined.pixel_data(0) == payload

    decoder = dicom.PixelPayloadDecoder(desc, split_payload)
    assert decoder.to_array(frame=0).tobytes() == payload


def test_split_pixeldata_payload_implicit_and_encapsulated_payloads() -> None:
    payload = b"\x34\x12\x56\x78\x9A\xBC"
    implicit_source = _build_native_implicit_full(payload)
    implicit_split = dicom.split_pixeldata_payload(
        [], implicit_source
    )
    assert implicit_split.ok is True
    implicit_main = implicit_split.main_bytes
    implicit_payload = implicit_split.pixel_payload
    assert implicit_payload == payload
    assert dicom.join_pixeldata_payload(implicit_main, implicit_payload) == implicit_source
    implicit_rt = dicom.read_bytes_with_pixeldata_payload(implicit_main, implicit_payload)
    assert implicit_rt.pixel_data(0) == payload

    encap_payload = _two_frame_encap_payload()
    encap_source = (
        _build_part10(
            "1.2.840.10008.1.2.1.98",
            _common_pixel_metadata(columns=2, bits_allocated=8, frames="2")
            + _pack_explicit_le(
                0x7FE0, 0x0010, "OB", encap_payload, undefined=True
            ),
        )
    )
    encap_split = dicom.split_pixeldata_payload([], encap_source)
    encap_main = encap_split.main_bytes
    encap_raw = encap_split.pixel_payload
    encap_desc = encap_split.decode_descriptor
    assert encap_raw == encap_payload
    assert encap_desc.frame_fragments
    assert dicom.join_pixeldata_payload(encap_main, encap_raw) == encap_source

    encap_rt = dicom.read_bytes_with_pixeldata_payload(encap_main, encap_raw)
    assert encap_rt.encoded_pixel_frame_bytes(0) == b"\x01\x02"
    assert encap_rt.encoded_pixel_frame_bytes(1) == b"\x03\x04"
    encap_decoder = dicom.PixelPayloadDecoder(encap_desc, encap_raw)
    assert encap_decoder.to_array(frame=0).tobytes() == b"\x01\x02"
    assert encap_decoder.to_array(frame=1).tobytes() == b"\x03\x04"


def test_split_pixeldata_payload_reports_unsupported_transfer_syntax() -> None:
    for tsuid in ("1.2.840.10008.1.2.2", "1.2.840.10008.1.2.1.99"):
        result = dicom.split_pixeldata_payload(
            [], _build_part10(tsuid, b"")
        )
        assert result.ok is False
        assert result.error_message
        assert result.main_bytes == b""
        assert result.pixel_payload == b""


def test_read_bytes_with_pixeldata_payload_native_detach_releases_owner() -> None:
    assert dicom.PIXELDATA_PAYLOAD_PLACEHOLDER_MAGIC == b"PIXDATA1"

    payload = bytearray(b"\x34\x12\x56\x78\x9A\xBC")
    obj = dicom.read_bytes_with_pixeldata_payload(
        _build_native_placeholder(), payload, name="py-split-native"
    )

    assert obj.path == "py-split-native"
    assert obj.has_attached_pixeldata_payload is True
    assert hasattr(obj, "_pixeldata_payload_owner")
    assert obj["PixelData"].vr == dicom.VR.OW
    assert obj.pixel_data(0) == bytes(payload)

    minimal = dicom.read_bytes_with_pixeldata_payload(
        _build_native_placeholder(), payload, name="py-split-native-minimal"
    )
    minimal.detach_pixeldata_payload()
    assert minimal.has_attached_pixeldata_payload is False
    assert not hasattr(minimal, "_pixeldata_payload_owner")
    assert minimal["PixelData"].value_bytes() == dicom.PIXELDATA_PAYLOAD_PLACEHOLDER_MAGIC
    assert "\\x34\\x12\\x56\\x78" not in minimal.dump()
    with pytest.raises(Exception, match="detached"):
        minimal.pixel_data(0)

    obj.detach_pixeldata_payload(keep_dump=True)
    assert obj.has_attached_pixeldata_payload is False
    assert not hasattr(obj, "_pixeldata_payload_owner")
    assert obj.Rows == 1
    marker = obj["PixelData"].value_bytes()
    assert marker.startswith(dicom.PIXELDATA_PAYLOAD_PLACEHOLDER_MAGIC)
    marker_text = marker[len(dicom.PIXELDATA_PAYLOAD_PLACEHOLDER_MAGIC) :].decode("utf-8")
    assert "'7fe00010'\tOW" in marker_text
    assert "\\x34\\x12\\x56\\x78" in marker_text
    assert "'7fe00010'\tOW" in obj.dump()
    assert "\\x34\\x12\\x56\\x78" in obj.dump()
    with pytest.raises(Exception, match="detached"):
        obj.pixel_data(0)


def test_read_bytes_with_pixeldata_payload_copy_false_keeps_buffers_alive() -> None:
    main_p10 = bytearray(_build_encap_placeholder("1"))
    payload = bytearray(_single_frame_encap_payload())

    obj = dicom.read_bytes_with_pixeldata_payload(
        main_p10, payload, name="py-split-encap", copy=False
    )
    del main_p10
    del payload
    gc.collect()

    assert obj.has_attached_pixeldata_payload is True
    assert obj.encoded_pixel_frame_bytes(0) == b"\x34\x12"
    assert obj.pixel_data(0) == b"\x34\x12"


def test_split_pixeldata_payload_join_and_reattach_roundtrip() -> None:
    payload = b"\x34\x12\x56\x78\x9A\xBC"
    native_source = _build_native_full(payload)
    native = dicom.split_pixeldata_payload(
        [], native_source, name="py-split-load-native"
    )
    assert native.ok is True
    main_bytes = native.main_bytes
    payload_bytes = native.pixel_payload
    assert payload_bytes == payload
    placeholder_only = dicom.read_bytes(main_bytes, name="py-split-load-native-main")
    assert _is_placeholder_metadata(placeholder_only["PixelData"].value_bytes())
    assert dicom.join_pixeldata_payload(main_bytes, payload_bytes) == native_source
    roundtrip = dicom.read_bytes_with_pixeldata_payload(
        main_bytes, payload_bytes, name="py-split-load-native-rt"
    )
    assert roundtrip.pixel_data(0) == payload

    encap_source = _build_part10(
        "1.2.840.10008.1.2.1.98",
        _common_pixel_metadata(columns=2, bits_allocated=8, frames="1")
        + _pack_explicit_le(
            0x7FE0, 0x0010, "OB", _single_frame_encap_payload(), undefined=True
        ),
    )
    encap = dicom.split_pixeldata_payload(
        [], encap_source, name="py-split-load-encap"
    )
    assert encap.ok is True
    main_bytes = encap.main_bytes
    payload_bytes = encap.pixel_payload
    placeholder_only = dicom.read_bytes(main_bytes, name="py-split-load-encap-main")
    assert _is_placeholder_metadata(placeholder_only["PixelData"].value_bytes())
    assert dicom.join_pixeldata_payload(main_bytes, payload_bytes) == encap_source
    roundtrip = dicom.read_bytes_with_pixeldata_payload(
        main_bytes, payload_bytes, name="py-split-load-encap-rt"
    )
    assert roundtrip.encoded_pixel_frame_bytes(0) == b"\x34\x12"
    assert roundtrip.pixel_data(0) == b"\x34\x12"


def test_pixel_payload_decoder_decodes_split_payload_only() -> None:
    payload = b"\x34\x12\x56\x78\x9A\xBC"
    native = dicom.split_pixeldata_payload(
        [], _build_native_full(payload), name="py-payload-decoder-native"
    )
    payload_bytes = native.pixel_payload
    desc = native.decode_descriptor
    assert desc.expected_payload_length == len(payload_bytes)
    assert desc.frame_fragments == ""

    decoder = dicom.PixelPayloadDecoder(desc, payload_bytes)
    plan = decoder.create_decode_plan()
    assert decoder.to_array(frame=0, plan=plan).tobytes() == payload
    out = np.empty((1, 3), dtype=np.uint16)
    returned = decoder.decode_into(out, frame=0, plan=plan)
    assert returned is out
    assert out.tobytes() == payload

    # Descriptor string_views are parsed at construction time; Python descriptor
    # strings can be replaced or released after the decoder is built.
    desc.transfer_syntax_uid = str(desc.transfer_syntax_uid)
    desc.photometric = str(desc.photometric)
    payload_owner = bytearray(payload_bytes)
    decoder = dicom.PixelPayloadDecoder(desc, payload_owner)
    with pytest.raises(BufferError):
        payload_owner.extend(b"\xAA")
    gc.collect()
    assert decoder.to_array(frame=0).tobytes() == payload

    encap_source = _build_part10(
        "1.2.840.10008.1.2.1.98",
        _common_pixel_metadata(columns=2, bits_allocated=8, frames="1")
        + _pack_explicit_le(
            0x7FE0, 0x0010, "OB", _single_frame_encap_payload(), undefined=True
        ),
    )
    encap = dicom.split_pixeldata_payload(
        [], encap_source, name="py-payload-decoder-encap"
    )
    encap_payload = encap.pixel_payload
    encap_desc = encap.decode_descriptor
    assert encap_desc.expected_payload_length == len(encap_payload)
    assert encap_desc.frame_fragments

    payload_owner = bytearray(encap_payload)
    encap_decoder = dicom.PixelPayloadDecoder(encap_desc, payload_owner)
    del payload_owner
    gc.collect()
    assert encap_decoder.to_array(frame=0).tobytes() == b"\x34\x12"

    bad_desc = _clone_descriptor(encap_desc)
    bad_desc.frame_fragments = ""
    with pytest.raises(Exception, match="frame_fragments"):
        dicom.PixelPayloadDecoder(bad_desc, encap_payload)

    bad_desc = _clone_descriptor(encap_desc)
    bad_desc.frame_fragments = "999999:1"
    with pytest.raises(Exception, match="outside pixel payload"):
        dicom.PixelPayloadDecoder(bad_desc, encap_payload)


def test_write_bytes_with_transfer_syntax_then_split_roundtrip() -> None:
    payload = b"\x34\x12\x56\x78\x9A\xBC"
    native = dicom.read_bytes_with_pixeldata_payload(
        _build_native_placeholder(), payload, name="py-split-transcode-native"
    )
    same_ts_bytes = native.write_bytes_with_transfer_syntax("ExplicitVRLittleEndian")
    same_ts_split = dicom.split_pixeldata_payload(
        [], same_ts_bytes, name="py-split-transcode-same"
    )
    main_bytes = same_ts_split.main_bytes
    payload_bytes = same_ts_split.pixel_payload
    assert payload_bytes == payload
    same_ts_roundtrip = dicom.read_bytes_with_pixeldata_payload(
        main_bytes, payload_bytes, name="py-split-transcode-same-rt"
    )
    assert same_ts_roundtrip.pixel_data(0) == payload

    big_endian_bytes = native.write_bytes_with_transfer_syntax("ExplicitVRBigEndian")
    big_endian_split = dicom.split_pixeldata_payload(
        [], big_endian_bytes, name="py-split-transcode-big-endian"
    )
    assert big_endian_split.ok is False
    assert big_endian_split.error_message
    assert big_endian_split.main_bytes == b""
    assert big_endian_split.pixel_payload == b""

    rle_bytes = native.write_bytes_with_transfer_syntax("RLELossless")
    rle_split = dicom.split_pixeldata_payload(
        [], rle_bytes, name="py-split-transcode-rle"
    )
    main_bytes = rle_split.main_bytes
    payload_bytes = rle_split.pixel_payload
    placeholder_only = dicom.read_bytes(main_bytes, name="py-split-transcode-rle-main")
    assert _is_placeholder_metadata(placeholder_only["PixelData"].value_bytes())
    rle_roundtrip = dicom.read_bytes_with_pixeldata_payload(
        main_bytes, payload_bytes, name="py-split-transcode-rle-rt"
    )
    assert rle_roundtrip.pixel_data(0) == payload

    ctx = dicom.create_encoder_context("RLELossless")
    rle_ctx_bytes = native.write_bytes_with_transfer_syntax(
        "RLELossless", encoder_context=ctx
    )
    rle_ctx_split = dicom.split_pixeldata_payload(
        [], rle_ctx_bytes, name="py-split-transcode-rle-ctx"
    )
    main_bytes = rle_ctx_split.main_bytes
    payload_bytes = rle_ctx_split.pixel_payload
    rle_roundtrip = dicom.read_bytes_with_pixeldata_payload(
        main_bytes, payload_bytes, name="py-split-transcode-rle-ctx-rt"
    )
    assert rle_roundtrip.pixel_data(0) == payload

    native_bytes = rle_roundtrip.write_bytes_with_transfer_syntax(
        "ExplicitVRLittleEndian"
    )
    native_split = dicom.split_pixeldata_payload(
        [], native_bytes, name="py-split-transcode-native-rt-main"
    )
    main_bytes = native_split.main_bytes
    payload_bytes = native_split.pixel_payload
    assert payload_bytes == payload
    native_roundtrip = dicom.read_bytes_with_pixeldata_payload(
        main_bytes, payload_bytes, name="py-split-transcode-native-rt"
    )
    assert native_roundtrip.pixel_data(0) == payload


def test_read_bytes_with_pixeldata_payload_keep_on_error_clears_attached_state() -> None:
    obj = dicom.read_bytes_with_pixeldata_payload(
        _build_native_placeholder(b"BAD!"),
        b"\x34\x12\x56\x78\x9A\xBC",
        name="py-split-bad-magic",
        keep_on_error=True,
    )

    assert obj.has_error is True
    assert obj.error_message
    assert obj.has_attached_pixeldata_payload is False
    assert not hasattr(obj, "_pixeldata_payload_owner")

    truncated = dicom.read_bytes_with_pixeldata_payload(
        _build_native_truncated_pixeldata_header(),
        b"\x34\x12\x56\x78\x9A\xBC",
        name="py-split-truncated-main",
        keep_on_error=True,
    )

    assert truncated.has_error is True
    assert truncated.error_message
    assert truncated.has_attached_pixeldata_payload is False
    assert not hasattr(truncated, "_pixeldata_payload_owner")
