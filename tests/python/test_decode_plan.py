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
    assert plan.options.worker_threads == -1
    assert plan.options.codec_threads == -1


def test_create_decode_plan_accepts_explicit_row_stride_and_ignores_alignment():
    dicom_file = dicom.read_file(_test_file())
    options = dicom.DecodeOptions(alignment=3, row_stride=16)

    plan = dicom_file.create_decode_plan(options)

    assert plan.row_stride == 16
    assert plan.frame_stride == 64
    assert plan.required_bytes(frame=0) == 64

    arr = dicom_file.to_array(frame=0, plan=plan)
    expected = dicom_file.to_array(frame=0)

    assert arr.shape == (4, 4)
    assert arr.strides == (16, 2)
    assert arr.flags.c_contiguous is False
    assert np.array_equal(arr, expected)


def test_create_decode_plan_accepts_explicit_frame_stride_with_packed_rows():
    dicom_file = dicom.read_file(_test_file())
    frame0 = np.arange(16, dtype=np.int16).reshape(4, 4)
    frame1 = (np.arange(16, dtype=np.int16) + 100).reshape(4, 4)
    source = np.stack([frame0, frame1], axis=0)
    dicom_file.set_pixel_data("ExplicitVRLittleEndian", source)

    options = dicom.DecodeOptions(alignment=3, frame_stride=48)
    plan = dicom_file.create_decode_plan(options)

    assert plan.row_stride == 8
    assert plan.frame_stride == 48
    assert plan.required_bytes(frame=-1) == 96

    arr = dicom_file.to_array(frame=-1, plan=plan)

    assert arr.shape == (2, 4, 4)
    assert arr.strides == (48, 8, 2)
    assert arr.flags.c_contiguous is False
    assert np.array_equal(arr, source)


def test_decode_into_accepts_raw_buffer_for_custom_stride_plan():
    dicom_file = dicom.read_file(_test_file())
    options = dicom.DecodeOptions(row_stride=16)
    plan = dicom_file.create_decode_plan(options)

    out = np.empty(
        plan.required_bytes(frame=0) // plan.bytes_per_sample,
        dtype=plan.dtype,
    )
    returned = dicom_file.decode_into(out, frame=0, plan=plan)

    view = np.ndarray(
        shape=plan.shape(frame=0),
        dtype=plan.dtype,
        buffer=out,
        strides=(plan.row_stride, plan.bytes_per_sample),
    )
    expected = dicom_file.to_array(frame=0)

    assert returned is out
    assert np.array_equal(view, expected)


def test_create_decode_plan_rejects_explicit_stride_smaller_than_payload():
    dicom_file = dicom.read_file(_test_file())

    with pytest.raises(RuntimeError, match="row_stride is smaller than row payload"):
        dicom_file.create_decode_plan(dicom.DecodeOptions(row_stride=6))

    with pytest.raises(RuntimeError, match="frame_stride is smaller than frame payload"):
        dicom_file.create_decode_plan(dicom.DecodeOptions(frame_stride=16))


def test_to_array_accepts_reusable_decode_plan():
    dicom_file = dicom.read_file(_test_file())
    options = dicom.DecodeOptions()
    plan = dicom_file.create_decode_plan(options)

    arr = dicom_file.to_array(frame=0, plan=plan)
    expected = dicom_file.to_array(frame=0)

    assert isinstance(arr, np.ndarray)
    assert arr.dtype == np.int16
    assert np.array_equal(arr, expected)


def test_decode_into_accepts_reusable_decode_plan():
    dicom_file = dicom.read_file(_test_file())
    options = dicom.DecodeOptions()
    plan = dicom_file.create_decode_plan(options)

    out = np.empty(plan.shape(frame=0), dtype=plan.dtype)
    returned = dicom_file.decode_into(out, frame=0, plan=plan)

    expected = dicom_file.to_array(frame=0)

    assert returned is out
    assert out.dtype == np.int16
    assert np.array_equal(out, expected)


def test_planar_decode_plan_exposes_plane_first_single_frame_array():
    dicom_file = dicom.read_file(_test_file())
    source = np.array(
        [
            [[1, 2, 3], [4, 5, 6]],
            [[7, 8, 9], [10, 11, 12]],
        ],
        dtype=np.uint8,
    )
    dicom_file.set_pixel_data("ExplicitVRLittleEndian", source)

    options = dicom.DecodeOptions(planar_out=dicom.Planar.planar)
    plan = dicom_file.create_decode_plan(options)

    assert plan.shape(frame=0) == (3, 2, 2)

    arr = dicom_file.to_array(frame=0, plan=plan)
    expected = np.transpose(source, (2, 0, 1))

    assert arr.shape == (3, 2, 2)
    assert arr.flags.c_contiguous
    assert np.array_equal(arr, expected)

    out = np.empty(plan.shape(frame=0), dtype=plan.dtype)
    returned = dicom_file.decode_into(out, frame=0, plan=plan)

    assert returned is out
    assert out.flags.c_contiguous
    assert np.array_equal(out, expected)


def test_planar_decode_plan_exposes_plane_first_multi_frame_array():
    dicom_file = dicom.read_file(_test_file())
    frame0 = np.array(
        [
            [[1, 2, 3], [4, 5, 6]],
            [[7, 8, 9], [10, 11, 12]],
        ],
        dtype=np.uint8,
    )
    frame1 = frame0 + np.uint8(20)
    source = np.stack([frame0, frame1], axis=0)
    dicom_file.set_pixel_data("ExplicitVRLittleEndian", source)

    options = dicom.DecodeOptions(planar_out=dicom.Planar.planar)
    plan = dicom_file.create_decode_plan(options)

    assert plan.shape(frame=0) == (3, 2, 2)
    assert plan.shape(frame=-1) == (2, 3, 2, 2)

    arr = dicom_file.to_array(frame=-1, plan=plan)
    expected = np.transpose(source, (0, 3, 1, 2))

    assert arr.shape == (2, 3, 2, 2)
    assert arr.flags.c_contiguous
    assert np.array_equal(arr, expected)

    out = np.empty(plan.shape(frame=-1), dtype=plan.dtype)
    returned = dicom_file.decode_into(out, frame=-1, plan=plan)

    assert returned is out
    assert out.flags.c_contiguous
    assert np.array_equal(out, expected)
