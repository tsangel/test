from pathlib import Path

import dicomsdl as dicom
import pytest

np = pytest.importorskip("numpy")


def _test_file(name: str = "test_le.dcm") -> str:
    return str(Path(__file__).resolve().parent.parent / name)


def _supports_encode(keyword: str) -> bool:
    for uid in dicom.transfer_syntax_uids_encode_supported():
        if (uid.keyword or uid.value) == keyword:
            return True
    return False


def test_decode_info_methods_exist():
    assert hasattr(dicom, "DecodeInfo")
    assert hasattr(dicom, "Photometric")
    assert hasattr(dicom, "EncodedLossyState")
    assert hasattr(dicom.DicomFile, "to_array")
    assert hasattr(dicom.DicomFile, "decode_into")
    assert not hasattr(dicom.DicomFile, "to_array_with_info")
    assert not hasattr(dicom.DicomFile, "decode_into_with_info")


def test_to_array_with_info_returns_array_and_decode_info():
    dicom_file = dicom.read_file(_test_file())

    arr, info = dicom_file.to_array(frame=0, with_info=True)

    assert isinstance(arr, np.ndarray)
    assert isinstance(info, dicom.DecodeInfo)
    assert arr.shape == (4, 4)
    assert arr.dtype == np.int16
    assert np.array_equal(arr, dicom_file.to_array(frame=0))
    assert info.photometric == dicom.Photometric.monochrome2
    assert info.encoded_lossy_state == dicom.EncodedLossyState.lossless
    assert info.dtype == np.dtype(np.int16)
    assert info.planar == dicom.Planar.interleaved
    assert info.bits_per_sample == 16


def test_decode_into_with_info_returns_decode_info_and_accepts_plan():
    dicom_file = dicom.read_file(_test_file())
    plan = dicom_file.create_decode_plan()
    out = np.empty(plan.shape(frame=0), dtype=plan.dtype)

    info = dicom_file.decode_into(out, frame=0, plan=plan, with_info=True)

    assert isinstance(info, dicom.DecodeInfo)
    assert np.array_equal(out, dicom_file.to_array(frame=0))
    assert info.photometric == dicom.Photometric.monochrome2
    assert info.encoded_lossy_state == dicom.EncodedLossyState.lossless
    assert info.dtype == np.dtype(np.int16)
    assert info.planar == dicom.Planar.interleaved
    assert info.bits_per_sample == 16


def test_to_array_with_info_frame_minus_one_reports_common_frame_zero_metadata():
    dicom_file = dicom.read_file(_test_file())
    frame0 = np.arange(16, dtype=np.int16).reshape(4, 4)
    frame1 = (np.arange(16, dtype=np.int16) + 100).reshape(4, 4)
    source = np.stack([frame0, frame1], axis=0)
    dicom_file.set_pixel_data("ExplicitVRLittleEndian", source)

    arr, info = dicom_file.to_array(frame=-1, with_info=True)

    assert np.array_equal(arr, source)
    assert info.photometric == dicom.Photometric.monochrome2
    assert info.encoded_lossy_state == dicom.EncodedLossyState.lossless
    assert info.dtype == np.dtype(np.int16)
    assert info.planar == dicom.Planar.interleaved
    assert info.bits_per_sample == 16


def test_pixel_array_with_info_matches_to_array_with_info():
    dicom_file = dicom.read_file(_test_file())

    arr, info = dicom_file.pixel_array(frame=0, with_info=True)

    assert isinstance(arr, np.ndarray)
    assert isinstance(info, dicom.DecodeInfo)
    assert np.array_equal(arr, dicom_file.to_array(frame=0))
    assert info.photometric == dicom.Photometric.monochrome2
    assert info.encoded_lossy_state == dicom.EncodedLossyState.lossless


@pytest.mark.parametrize(
    ("photometric_text", "source_shape", "expected_photometric"),
    [
        ("YBR_RCT", (2, 3, 3), dicom.Photometric.ybr_rct),
        ("YBR_ICT", (2, 3, 3), dicom.Photometric.ybr_ict),
        ("XYB", (2, 3, 3), dicom.Photometric.xyb),
        ("HSV", (2, 3, 3), dicom.Photometric.hsv),
    ],
)
def test_to_array_with_info_reports_representable_photometric_values(
    photometric_text: str, source_shape: tuple[int, ...], expected_photometric
):
    dicom_file = dicom.read_file(_test_file())
    source = np.arange(np.prod(source_shape), dtype=np.uint8).reshape(source_shape)
    dicom_file.set_pixel_data("ExplicitVRLittleEndian", source)
    assert dicom_file.set_value("PhotometricInterpretation", photometric_text)

    arr, info = dicom_file.to_array(frame=0, with_info=True)

    assert np.array_equal(arr, source)
    assert info.photometric == expected_photometric


def test_decode_into_with_info_reports_rgb_for_native_color_planar_conversion():
    dicom_file = dicom.read_file(_test_file())
    source = np.arange(18, dtype=np.uint8).reshape(2, 3, 3)
    dicom_file.set_pixel_data("ExplicitVRLittleEndian", source)

    plan = dicom_file.create_decode_plan(
        dicom.DecodeOptions(planar_out=dicom.Planar.planar)
    )
    out = np.empty(plan.shape(frame=0), dtype=plan.dtype)

    info = dicom_file.decode_into(out, frame=0, plan=plan, with_info=True)

    assert info.photometric == dicom.Photometric.rgb
    assert info.encoded_lossy_state == dicom.EncodedLossyState.lossless


@pytest.mark.parametrize(
    ("transfer_syntax", "expected_photometric"),
    [
        ("RLELossless", dicom.Photometric.rgb),
        ("JPEGLSLossless", dicom.Photometric.rgb),
        ("JPEG2000Lossless", dicom.Photometric.ybr_rct),
        ("HTJ2KLossless", dicom.Photometric.ybr_rct),
    ],
)
def test_to_array_with_info_reports_photometric_for_color_lossless_codecs(
    transfer_syntax: str, expected_photometric
):
    if not _supports_encode(transfer_syntax):
        pytest.skip(f"{transfer_syntax} encoder is not available in this build")

    dicom_file = dicom.read_file(_test_file())
    source = np.arange(18, dtype=np.uint8).reshape(2, 3, 3)
    dicom_file.set_pixel_data(transfer_syntax, source)

    arr, info = dicom_file.to_array(frame=0, with_info=True)

    assert np.array_equal(arr, source)
    assert info.photometric == expected_photometric
