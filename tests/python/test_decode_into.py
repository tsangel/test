from pathlib import Path

import dicomsdl as dicom
import pytest

np = pytest.importorskip("numpy")


def _test_file(name: str = "test_le.dcm") -> str:
    return str(Path(__file__).resolve().parent.parent / name)


def test_decode_into_method_exists():
    assert hasattr(dicom.DataSet, "decode_into")


def test_decode_into_matches_to_array_single_frame():
    ds = dicom.read_file(_test_file())

    out = np.empty((4, 4), dtype=np.int16)
    returned = ds.decode_into(out, frame=0, scaled=False)

    assert returned is out
    assert np.array_equal(out, ds.to_array(frame=0, scaled=False))


def test_decode_into_scaled_matches_to_array():
    ds = dicom.read_file(_test_file())

    out = np.empty((4, 4), dtype=np.float32)
    ds.decode_into(out, frame=0, scaled=True)

    expected = ds.to_array(frame=0, scaled=True)
    assert np.array_equal(out, expected)


def test_decode_into_threads_option_accepted():
    ds = dicom.read_file(_test_file())

    out = np.empty((4, 4), dtype=np.int16)
    ds.decode_into(out, frame=0, scaled=False, threads=1)

    expected = ds.to_array(frame=0, scaled=False)
    assert np.array_equal(out, expected)


def test_decode_into_invalid_frame():
    ds = dicom.read_file(_test_file())
    out = np.empty((4, 4), dtype=np.int16)

    with pytest.raises(IndexError):
        ds.decode_into(out, frame=1)

    with pytest.raises(ValueError):
        ds.decode_into(out, frame=-2)

    with pytest.raises(ValueError):
        ds.decode_into(out, frame=0, threads=-2)


def test_decode_into_size_mismatch_raises():
    ds = dicom.read_file(_test_file())
    out = np.empty((4, 3), dtype=np.int16)

    with pytest.raises(ValueError):
        ds.decode_into(out, frame=0, scaled=False)


def test_decode_into_requires_writable_c_contiguous_buffer():
    ds = dicom.read_file(_test_file())

    readonly = np.empty((4, 4), dtype=np.int16)
    readonly.flags.writeable = False
    with pytest.raises(TypeError):
        ds.decode_into(readonly, frame=0, scaled=False)

    non_contiguous = np.empty((4, 4), dtype=np.int16)[:, ::2]
    with pytest.raises(TypeError):
        ds.decode_into(non_contiguous, frame=0, scaled=False)
