from pathlib import Path

import dicomsdl as dicom
import pytest

np = pytest.importorskip("numpy")


def _test_file(name: str = "test_le.dcm") -> str:
    return str(Path(__file__).resolve().parent.parent / name)


def test_set_pixel_data_method_exists():
    assert hasattr(dicom.DicomFile, "set_pixel_data")


def test_set_pixel_data_native_single_frame_roundtrip():
    dicom_file = dicom.read_file(_test_file())
    source = np.arange(16, dtype=np.int16).reshape(4, 4)

    dicom_file.set_pixel_data("ExplicitVRLittleEndian", source)

    decoded = dicom_file.to_array(frame=0)
    assert np.array_equal(decoded, source)
    assert dicom_file.transfer_syntax_uid.keyword == "ExplicitVRLittleEndian"
    assert not dicom_file.get_dataelement("PixelData").is_pixel_sequence


def test_set_pixel_data_native_multi_frame_roundtrip():
    dicom_file = dicom.read_file(_test_file())
    frame0 = np.full((4, 4), 3, dtype=np.int16)
    frame1 = np.full((4, 4), 7, dtype=np.int16)
    source = np.stack([frame0, frame1], axis=0)

    dicom_file.set_pixel_data("ExplicitVRLittleEndian", source)

    decoded = dicom_file.to_array(frame=-1)
    assert decoded.shape == (2, 4, 4)
    assert np.array_equal(decoded, source)
    assert np.array_equal(dicom_file.to_array(frame=0), frame0)
    assert np.array_equal(dicom_file.to_array(frame=1), frame1)


def test_set_pixel_data_rejects_non_contiguous_source():
    dicom_file = dicom.read_file(_test_file())
    source = np.arange(32, dtype=np.int16).reshape(4, 8)[:, ::2]

    with pytest.raises(TypeError, match="C-contiguous buffer object"):
        dicom_file.set_pixel_data("ExplicitVRLittleEndian", source)


def test_set_pixel_data_rejects_unsupported_dtype():
    dicom_file = dicom.read_file(_test_file())
    source = np.zeros((4, 4), dtype=np.bool_)

    with pytest.raises(ValueError, match="unsupported source dtype"):
        dicom_file.set_pixel_data("ExplicitVRLittleEndian", source)


def test_set_pixel_data_rejects_invalid_ndim():
    dicom_file = dicom.read_file(_test_file())
    source = np.array([1, 2, 3], dtype=np.int16)

    with pytest.raises(ValueError, match="ndim 2, 3, or 4"):
        dicom_file.set_pixel_data("ExplicitVRLittleEndian", source)


def test_set_pixel_data_rejects_invalid_options_type():
    dicom_file = dicom.read_file(_test_file())
    source = np.zeros((4, 4), dtype=np.int16)

    with pytest.raises(TypeError, match="options must be None, str, or dict"):
        dicom_file.set_pixel_data("ExplicitVRLittleEndian", source, options=123)


def test_set_pixel_data_rejects_incompatible_option_type():
    dicom_file = dicom.read_file(_test_file())
    source = np.zeros((4, 4), dtype=np.int16)

    with pytest.raises(ValueError, match="incompatible with transfer syntax"):
        dicom_file.set_pixel_data("ExplicitVRLittleEndian", source, options="j2k")


def test_set_pixel_data_supports_encoder_context():
    dicom_file = dicom.read_file(_test_file())
    source = np.arange(16, dtype=np.int16).reshape(4, 4)
    ctx = dicom.create_encoder_context("ExplicitVRLittleEndian")

    dicom_file.set_pixel_data(
        "ExplicitVRLittleEndian", source, encoder_context=ctx
    )

    decoded = dicom_file.to_array(frame=0)
    assert np.array_equal(decoded, source)


def test_set_pixel_data_rejects_encoder_context_transfer_syntax_mismatch():
    dicom_file = dicom.read_file(_test_file())
    source = np.zeros((4, 4), dtype=np.int16)
    ctx = dicom.create_encoder_context("RLELossless")

    with pytest.raises(RuntimeError, match="encoder context transfer syntax mismatch"):
        dicom_file.set_pixel_data(
            "ExplicitVRLittleEndian", source, encoder_context=ctx
        )


def test_set_pixel_data_replaces_one_encapsulated_frame_with_frame_index():
    supported = {
        uid.keyword or uid.value for uid in dicom.transfer_syntax_uids_encode_supported()
    }
    if "RLELossless" not in supported:
        pytest.skip("RLELossless encoder is not available in this build")

    dicom_file = dicom.read_file(_test_file())
    frame0 = np.full((4, 4), 3, dtype=np.int16)
    frame1 = np.full((4, 4), 7, dtype=np.int16)
    dicom_file.set_pixel_data("RLELossless", np.stack([frame0, frame1], axis=0))

    replacement = np.full((4, 4), 19, dtype=np.int16)
    dicom_file.set_pixel_data("RLELossless", replacement, frame_index=1)

    decoded = dicom_file.to_array(frame=-1)
    assert decoded.shape == (2, 4, 4)
    assert np.array_equal(decoded[0], frame0)
    assert np.array_equal(decoded[1], replacement)
    assert dicom_file.transfer_syntax_uid.keyword == "RLELossless"


def test_set_pixel_data_frame_index_supports_encoder_context():
    supported = {
        uid.keyword or uid.value for uid in dicom.transfer_syntax_uids_encode_supported()
    }
    if "RLELossless" not in supported:
        pytest.skip("RLELossless encoder is not available in this build")

    dicom_file = dicom.read_file(_test_file())
    frame0 = np.arange(16, dtype=np.int16).reshape(4, 4)
    frame1 = np.full((4, 4), 5, dtype=np.int16)
    dicom_file.set_pixel_data("RLELossless", np.stack([frame0, frame1], axis=0))

    ctx = dicom.create_encoder_context("RLELossless")
    replacement = np.full((4, 4), 23, dtype=np.int16)
    dicom_file.set_pixel_data(
        "RLELossless", replacement, frame_index=0, encoder_context=ctx
    )

    decoded = dicom_file.to_array(frame=-1)
    assert np.array_equal(decoded[0], replacement)
    assert np.array_equal(decoded[1], frame1)


def test_set_pixel_data_frame_index_recomputes_lossy_ratio_for_fragment_backed_frames():
    supported = {
        uid.keyword or uid.value for uid in dicom.transfer_syntax_uids_encode_supported()
    }
    if "JPEG2000" not in supported:
        pytest.skip("JPEG2000 encoder is not available in this build")

    dicom_file = dicom.read_file(_test_file())
    frame0 = np.arange(32 * 32, dtype=np.int16).reshape(32, 32)
    frame1 = np.full((32, 32), 17, dtype=np.int16)
    source = np.stack([frame0, frame1], axis=0)
    dicom_file.set_pixel_data("JPEG2000", source)

    roundtrip = dicom.read_bytes(
        dicom_file.write_bytes(), name="single-frame-lossy-roundtrip"
    )
    original_frame0_bytes = roundtrip.encoded_pixel_frame_bytes(0)
    replacement = np.full((32, 32), 511, dtype=np.int16)
    roundtrip.set_pixel_data("JPEG2000", replacement, frame_index=0)
    replaced_frame0_bytes = roundtrip.encoded_pixel_frame_bytes(0)
    assert replaced_frame0_bytes != original_frame0_bytes

    encoded_total = len(replaced_frame0_bytes) + len(
        roundtrip.encoded_pixel_frame_bytes(1)
    )
    ratios = (
        roundtrip.get_dataelement("LossyImageCompressionRatio").to_double_vector()
    )
    assert ratios is not None and len(ratios) >= 2
    assert ratios[-1] == pytest.approx(source.nbytes / encoded_total, rel=1e-6)


def test_set_pixel_data_frame_index_rejects_multi_frame_source():
    supported = {
        uid.keyword or uid.value for uid in dicom.transfer_syntax_uids_encode_supported()
    }
    if "RLELossless" not in supported:
        pytest.skip("RLELossless encoder is not available in this build")

    dicom_file = dicom.read_file(_test_file())
    frame0 = np.full((4, 4), 1, dtype=np.int16)
    frame1 = np.full((4, 4), 2, dtype=np.int16)
    dicom_file.set_pixel_data("RLELossless", np.stack([frame0, frame1], axis=0))

    with pytest.raises(RuntimeError, match="single-frame set_pixel_data"):
        dicom_file.set_pixel_data(
            "RLELossless", np.stack([frame0, frame1], axis=0), frame_index=0
        )


def test_set_pixel_data_frame_index_rejects_native_transfer_syntax():
    dicom_file = dicom.read_file(_test_file())
    source = np.arange(16, dtype=np.int16).reshape(4, 4)

    with pytest.raises(
        RuntimeError, match="requires an encapsulated target transfer syntax"
    ):
        dicom_file.set_pixel_data(
            "ExplicitVRLittleEndian", source, frame_index=0
        )


def test_set_pixel_data_jpeg_ybr_defaults_to_ybr_full_422_photometric():
    supported = {
        uid.keyword or uid.value for uid in dicom.transfer_syntax_uids_encode_supported()
    }
    if "JPEGBaseline8Bit" not in supported:
        pytest.skip("JPEGBaseline8Bit encoder is not available in this build")

    dicom_file = dicom.read_file(_test_file())
    source = np.arange(2 * 3 * 3, dtype=np.uint8).reshape(2, 3, 3)

    dicom_file.set_pixel_data(
        "JPEGBaseline8Bit",
        source,
        options={"type": "jpeg", "quality": 90, "color_space": "ybr"},
    )

    assert (
        dicom_file.get_dataelement("PhotometricInterpretation").to_string_view()
        == "YBR_FULL_422"
    )


def test_set_pixel_data_frame_index_supports_jpeg_ybr_full_422_target():
    supported = {
        uid.keyword or uid.value for uid in dicom.transfer_syntax_uids_encode_supported()
    }
    if "JPEGBaseline8Bit" not in supported:
        pytest.skip("JPEGBaseline8Bit encoder is not available in this build")

    dicom_file = dicom.read_file(_test_file())
    frame0 = np.arange(4 * 4 * 3, dtype=np.uint8).reshape(4, 4, 3)
    frame1 = np.full((4, 4, 3), 91, dtype=np.uint8)
    options = {
        "type": "jpeg",
        "quality": 90,
        "color_space": "ybr",
        "subsampling": "422",
    }

    dicom_file.set_pixel_data(
        "JPEGBaseline8Bit", np.stack([frame0, frame1], axis=0), options=options
    )
    original_frame1 = dicom_file.encoded_pixel_frame_bytes(1)
    replacement = np.full((4, 4, 3), 17, dtype=np.uint8)
    dicom_file.set_pixel_data(
        "JPEGBaseline8Bit", replacement, frame_index=1, options=options
    )

    assert (
        dicom_file.get_dataelement("PhotometricInterpretation").to_string_view()
        == "YBR_FULL_422"
    )
    assert dicom_file.encoded_pixel_frame_bytes(1) != original_frame1
    assert dicom_file.to_array(frame=-1).shape == (2, 4, 4, 3)

    with pytest.raises(RuntimeError, match="target pixel metadata"):
        dicom_file.set_pixel_data(
            "JPEGBaseline8Bit",
            replacement,
            frame_index=0,
            options={"type": "jpeg", "quality": 90},
        )


def test_set_pixel_data_rejects_jpeg_ybr_subsampling_444():
    supported = {
        uid.keyword or uid.value for uid in dicom.transfer_syntax_uids_encode_supported()
    }
    if "JPEGBaseline8Bit" not in supported:
        pytest.skip("JPEGBaseline8Bit encoder is not available in this build")

    dicom_file = dicom.read_file(_test_file())
    source = np.arange(2 * 2 * 3, dtype=np.uint8).reshape(2, 2, 3)

    with pytest.raises(ValueError, match="subsampling=422"):
        dicom_file.set_pixel_data(
            "JPEGBaseline8Bit",
            source,
            options={
                "type": "jpeg",
                "quality": 90,
                "color_space": "ybr",
                "subsampling": "444",
            },
        )


def test_set_pixel_data_rejects_jpeg_extended12bit_color_space_options():
    supported = {
        uid.keyword or uid.value for uid in dicom.transfer_syntax_uids_encode_supported()
    }
    if "JPEGExtended12Bit" not in supported:
        pytest.skip("JPEGExtended12Bit encoder is not available in this build")

    dicom_file = dicom.read_file(_test_file())
    source = np.arange(2 * 2 * 3, dtype=np.uint8).reshape(2, 2, 3)

    with pytest.raises(ValueError, match="JPEGBaseline8Bit"):
        dicom_file.set_pixel_data(
            "JPEGExtended12Bit",
            source,
            options={
                "type": "jpeg",
                "quality": 90,
                "color_space": "ybr",
            },
        )
