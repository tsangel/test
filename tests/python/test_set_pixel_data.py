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
