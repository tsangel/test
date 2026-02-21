from pathlib import Path

import dicomsdl as dicom
import pytest

np = pytest.importorskip("numpy")
ImageModule = pytest.importorskip("PIL.Image")


def _test_file(name: str = "test_le.dcm") -> str:
    return str(Path(__file__).resolve().parent.parent / name)


def test_to_pil_image_method_exists():
    assert hasattr(dicom.DataSet, "to_pil_image")


def test_to_pil_image_single_frame_default():
    dicom_file = dicom.read_file(_test_file())
    ds = dicom_file.dataset
    image = ds.to_pil_image(frame=0)

    assert isinstance(image, ImageModule.Image)
    assert image.mode == "L"
    assert image.size == (4, 4)

    arr = np.asarray(image)
    assert arr.shape == (4, 4)
    assert arr.dtype == np.uint8


def test_to_pil_image_frame_validation():
    dicom_file = dicom.read_file(_test_file())
    ds = dicom_file.dataset

    with pytest.raises(ValueError):
        ds.to_pil_image(frame=-1)

    with pytest.raises(IndexError):
        ds.to_pil_image(frame=1)


def test_to_pil_image_explicit_window():
    dicom_file = dicom.read_file(_test_file())
    ds = dicom_file.dataset
    image = ds.to_pil_image(window=(0.0, 400.0))

    arr = np.asarray(image)
    assert arr.shape == (4, 4)
    assert arr.dtype == np.uint8
