from pathlib import Path

import dicomsdl as dicom
import pytest

np = pytest.importorskip("numpy")


def _test_file(name: str = "test_le.dcm") -> str:
    return str(Path(__file__).resolve().parent.parent / name)


def test_to_array_single_frame():
    dicom_file = dicom.read_file(_test_file())

    arr = dicom_file.to_array()
    arr0 = dicom_file.to_array(frame=0)

    assert isinstance(arr, np.ndarray)
    assert arr.shape == (4, 4)
    assert arr.dtype == np.int16
    assert np.array_equal(arr, arr0)
    assert np.all(arr == 1)


def test_to_array_scaled():
    dicom_file = dicom.read_file(_test_file())
    arr = dicom_file.to_array(scaled=True)

    assert isinstance(arr, np.ndarray)
    assert arr.shape == (4, 4)
    assert arr.dtype == np.float32
    assert np.allclose(arr, 1.0)


def test_to_array_scaled_ignored_without_modality_transform():
    dicom_file = dicom.read_file(_test_file())
    ds = dicom_file.dataset
    ds.remove_dataelement(dicom.Tag("RescaleSlope"))
    ds.remove_dataelement(dicom.Tag("RescaleIntercept"))

    arr = dicom_file.to_array(scaled=True)
    assert isinstance(arr, np.ndarray)
    assert arr.shape == (4, 4)
    assert arr.dtype == np.int16


def test_to_array_invalid_frame():
    dicom_file = dicom.read_file(_test_file())

    with pytest.raises(IndexError):
        dicom_file.to_array(frame=1)

    with pytest.raises(ValueError):
        dicom_file.to_array(frame=-2)


def test_to_array_pixel_array_alias():
    dicom_file = dicom.read_file(_test_file())
    via_to_array = dicom_file.to_array(frame=0)
    via_alias = dicom_file.pixel_array(frame=0)
    assert np.array_equal(via_to_array, via_alias)
