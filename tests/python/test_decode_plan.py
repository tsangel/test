from pathlib import Path

import dicomsdl as dicom
import pytest

np = pytest.importorskip("numpy")


def _test_file(name: str = "test_le.dcm") -> str:
    return str(Path(__file__).resolve().parent.parent / name)


def test_create_decode_plan_exposes_shape_dtype_and_strides():
    dicom_file = dicom.read_file(_test_file())

    plan = dicom_file.create_decode_plan()

    assert plan.has_pixel_data is True
    assert plan.rows == 4
    assert plan.cols == 4
    assert plan.frames == 1
    assert plan.samples_per_pixel == 1
    assert plan.shape(frame=0) == (4, 4)
    assert plan.shape(frame=-1) == (4, 4)
    assert plan.dtype == np.dtype(np.int16)
    assert plan.bytes_per_sample == 2
    assert plan.row_stride == 8
    assert plan.frame_stride == 32
    assert plan.required_bytes(frame=0) == 32
    assert plan.options.planar_out == dicom.Planar.interleaved
    assert plan.options.to_modality_value is False
    assert plan.options.worker_threads == -1
    assert plan.options.codec_threads == -1


def test_to_array_accepts_reusable_decode_plan():
    dicom_file = dicom.read_file(_test_file())
    options = dicom.DecodeOptions(to_modality_value=True)
    plan = dicom_file.create_decode_plan(options)

    arr = dicom_file.to_array(frame=0, plan=plan)
    expected = dicom_file.to_array(frame=0, to_modality_value=True)

    assert isinstance(arr, np.ndarray)
    assert arr.dtype == np.float32
    assert np.array_equal(arr, expected)


def test_decode_into_accepts_reusable_decode_plan():
    dicom_file = dicom.read_file(_test_file())
    options = dicom.DecodeOptions(to_modality_value=True)
    plan = dicom_file.create_decode_plan(options)

    out = np.empty(plan.shape(frame=0), dtype=plan.dtype)
    returned = dicom_file.decode_into(out, frame=0, plan=plan)

    expected = dicom_file.to_array(frame=0, to_modality_value=True)

    assert returned is out
    assert out.dtype == np.float32
    assert np.array_equal(out, expected)
