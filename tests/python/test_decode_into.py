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


def test_decode_into_method_exists():
    assert hasattr(dicom.DicomFile, "decode_into")


def test_decode_into_matches_to_array_single_frame():
    dicom_file = dicom.read_file(_test_file())

    out = np.empty((4, 4), dtype=np.int16)
    returned = dicom_file.decode_into(out, frame=0)

    assert returned is out
    assert np.array_equal(out, dicom_file.to_array(frame=0))


def test_decode_into_worker_threads_option_accepted():
    dicom_file = dicom.read_file(_test_file())

    out = np.empty((4, 4), dtype=np.int16)
    dicom_file.decode_into(out, frame=0, worker_threads=1)

    expected = dicom_file.to_array(frame=0)
    assert np.array_equal(out, expected)


def test_decode_into_invalid_frame():
    dicom_file = dicom.read_file(_test_file())
    out = np.empty((4, 4), dtype=np.int16)

    with pytest.raises(IndexError):
        dicom_file.decode_into(out, frame=1)

    with pytest.raises(ValueError):
        dicom_file.decode_into(out, frame=-2)

    with pytest.raises(ValueError):
        dicom_file.decode_into(out, frame=0, worker_threads=-2)

    with pytest.raises(ValueError):
        dicom_file.decode_into(out, frame=0, codec_threads=-2)


def test_to_array_and_pixel_array_reject_invalid_thread_options_on_direct_path():
    dicom_file = dicom.read_file(_test_file())

    with pytest.raises(ValueError):
        dicom_file.to_array(frame=0, worker_threads=-2)

    with pytest.raises(ValueError):
        dicom_file.to_array(frame=0, codec_threads=-2)

    with pytest.raises(ValueError):
        dicom_file.pixel_array(frame=0, worker_threads=-2)

    with pytest.raises(ValueError):
        dicom_file.pixel_array(frame=0, codec_threads=-2)


def test_decode_into_size_mismatch_raises():
    dicom_file = dicom.read_file(_test_file())
    out = np.empty((4, 3), dtype=np.int16)

    with pytest.raises(ValueError):
        dicom_file.decode_into(out, frame=0)


def test_decode_into_requires_writable_c_contiguous_buffer():
    dicom_file = dicom.read_file(_test_file())

    readonly = np.empty((4, 4), dtype=np.int16)
    readonly.flags.writeable = False
    with pytest.raises(TypeError):
        dicom_file.decode_into(readonly, frame=0)

    non_contiguous = np.empty((4, 4), dtype=np.int16)[:, ::2]
    with pytest.raises(TypeError):
        dicom_file.decode_into(non_contiguous, frame=0)


def test_decode_into_frame_minus_one_single_frame_matches_frame_zero():
    dicom_file = dicom.read_file(_test_file())
    out = np.empty((4, 4), dtype=np.int16)
    returned = dicom_file.decode_into(out, frame=-1)
    assert returned is out
    assert np.array_equal(out, dicom_file.to_array(frame=0))


def test_decode_into_frame_minus_one_multi_frame_threads_roundtrip():
    dicom_file = dicom.read_file(_test_file())
    frame0 = np.arange(16, dtype=np.int16).reshape(4, 4)
    frame1 = (np.arange(16, dtype=np.int16) + 100).reshape(4, 4)
    frame2 = (np.arange(16, dtype=np.int16) + 200).reshape(4, 4)
    source = np.stack([frame0, frame1, frame2], axis=0)

    dicom_file.set_pixel_data("ExplicitVRLittleEndian", source)

    out = np.empty_like(source)
    returned = dicom_file.decode_into(out, frame=-1, worker_threads=2)

    assert returned is out
    assert np.array_equal(out, source)


def test_decode_into_single_frame_default_auto_roundtrip_jpeg2000_lossless():
    if not _supports_encode("JPEG2000Lossless"):
        pytest.skip("JPEG2000Lossless encoder is not available in this build")

    dicom_file = dicom.read_file(_test_file())
    source = np.arange(16, dtype=np.int16).reshape(4, 4)
    dicom_file.set_pixel_data("JPEG2000Lossless", source)

    out = np.empty_like(source)
    returned = dicom_file.decode_into(out, frame=0)

    assert returned is out
    assert np.array_equal(out, source)


def test_decode_into_frame_minus_one_default_auto_roundtrip_jpeg2000_lossless():
    if not _supports_encode("JPEG2000Lossless"):
        pytest.skip("JPEG2000Lossless encoder is not available in this build")

    dicom_file = dicom.read_file(_test_file())
    frame0 = np.arange(16, dtype=np.int16).reshape(4, 4)
    frame1 = (np.arange(16, dtype=np.int16) + 100).reshape(4, 4)
    source = np.stack([frame0, frame1], axis=0)
    dicom_file.set_pixel_data("JPEG2000Lossless", source)

    out = np.empty_like(source)
    returned = dicom_file.decode_into(out, frame=-1)

    assert returned is out
    assert np.array_equal(out, source)
