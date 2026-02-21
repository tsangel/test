from pathlib import Path

import dicomsdl as dicom
import pytest

np = pytest.importorskip("numpy")


def _test_file(name: str = "test_le.dcm") -> str:
    return str(Path(__file__).resolve().parent.parent / name)


def test_decode_into_method_exists():
    assert hasattr(dicom.DicomFile, "decode_into")


def test_decode_into_matches_to_array_single_frame():
    dicom_file = dicom.read_file(_test_file())

    out = np.empty((4, 4), dtype=np.int16)
    returned = dicom_file.decode_into(out, frame=0, scaled=False)

    assert returned is out
    assert np.array_equal(out, dicom_file.to_array(frame=0, scaled=False))


def test_decode_into_scaled_matches_to_array():
    dicom_file = dicom.read_file(_test_file())

    out = np.empty((4, 4), dtype=np.float32)
    dicom_file.decode_into(out, frame=0, scaled=True)

    expected = dicom_file.to_array(frame=0, scaled=True)
    assert np.array_equal(out, expected)


def test_decode_into_threads_option_accepted():
    dicom_file = dicom.read_file(_test_file())

    out = np.empty((4, 4), dtype=np.int16)
    dicom_file.decode_into(out, frame=0, scaled=False, threads=1)

    expected = dicom_file.to_array(frame=0, scaled=False)
    assert np.array_equal(out, expected)


def test_decode_into_invalid_frame():
    dicom_file = dicom.read_file(_test_file())
    out = np.empty((4, 4), dtype=np.int16)

    with pytest.raises(IndexError):
        dicom_file.decode_into(out, frame=1)

    with pytest.raises(ValueError):
        dicom_file.decode_into(out, frame=-2)

    with pytest.raises(ValueError):
        dicom_file.decode_into(out, frame=0, threads=-2)


def test_decode_into_size_mismatch_raises():
    dicom_file = dicom.read_file(_test_file())
    out = np.empty((4, 3), dtype=np.int16)

    with pytest.raises(ValueError):
        dicom_file.decode_into(out, frame=0, scaled=False)


def test_decode_into_requires_writable_c_contiguous_buffer():
    dicom_file = dicom.read_file(_test_file())

    readonly = np.empty((4, 4), dtype=np.int16)
    readonly.flags.writeable = False
    with pytest.raises(TypeError):
        dicom_file.decode_into(readonly, frame=0, scaled=False)

    non_contiguous = np.empty((4, 4), dtype=np.int16)[:, ::2]
    with pytest.raises(TypeError):
        dicom_file.decode_into(non_contiguous, frame=0, scaled=False)
