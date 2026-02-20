from pathlib import Path

import dicomsdl as dicom
import pytest

np = pytest.importorskip("numpy")


def _test_file(name: str = "test_le.dcm") -> str:
    return str(Path(__file__).resolve().parent.parent / name)


def test_to_array_view_method_exists():
    assert hasattr(dicom.DataSet, "to_array_view")


def test_to_array_view_matches_to_array():
    ds = dicom.read_file(_test_file())
    arr_view = ds.to_array_view(frame=0)
    arr_copy = ds.to_array(frame=0, scaled=False)

    assert isinstance(arr_view, np.ndarray)
    assert arr_view.shape == arr_copy.shape
    assert arr_view.dtype == arr_copy.dtype
    assert np.array_equal(arr_view, arr_copy)


def test_to_array_view_is_readonly():
    ds = dicom.read_file(_test_file())
    arr_view = ds.to_array_view(frame=0)
    assert not arr_view.flags.writeable

    with pytest.raises(ValueError):
        arr_view[0, 0] = 7


def test_to_array_view_frame_validation():
    ds = dicom.read_file(_test_file())

    with pytest.raises(IndexError):
        ds.to_array_view(frame=1)

    with pytest.raises(ValueError):
        ds.to_array_view(frame=-2)
