from __future__ import annotations

import shutil
from pathlib import Path

import dicomsdl as dicom
import numpy as np
import pytest


REPO_ROOT = Path(__file__).resolve().parents[2]
MULTIFRAME_ROOT = REPO_ROOT.parent / "sample" / "nema" / "Multiframe"
GEOM_ATOL = 1e-4
VOXEL_RTOL = 1e-5
VOXEL_ATOL = 1e-3


def _require_multiframe_file(relative_path: str) -> Path:
    path = MULTIFRAME_ROOT / relative_path
    if not path.is_file():
        pytest.skip(f"Multiframe sample is not available: {path}")
    return path


def _load_simpleitk_reference(path: Path):
    sitk = pytest.importorskip("SimpleITK")
    image = sitk.ReadImage(str(path))
    array = np.array(sitk.GetArrayViewFromImage(image), copy=True)
    return sitk, image, array


def _canonical_multiframe_reference(path: Path, reference_array: np.ndarray):
    dicom_file = dicom.read_file(path)

    def _unit(vec: np.ndarray) -> np.ndarray:
        return vec / np.linalg.norm(vec)

    def _fg_value(frame_index: int, sequence_name: str, value_name: str):
        try:
            value = dicom_file.get_value(
                f"PerFrameFunctionalGroupsSequence.{frame_index}.{sequence_name}.0.{value_name}",
                default=None,
            )
        except Exception:
            value = None
        if value is not None:
            return value

        try:
            value = dicom_file.get_value(
                f"SharedFunctionalGroupsSequence.0.{sequence_name}.0.{value_name}",
                default=None,
            )
        except Exception:
            value = None
        if value is not None:
            return value

        return dicom_file.get_value(value_name, default=None)

    frame_count = int(dicom_file.get_value("NumberOfFrames"))
    orientation = [float(value) for value in _fg_value(0, "PlaneOrientationSequence", "ImageOrientationPatient")]
    row = _unit(np.asarray(orientation[:3], dtype=np.float64))
    col = _unit(np.asarray(orientation[3:], dtype=np.float64))
    normal = _unit(np.cross(row, col))
    pixel_spacing = tuple(
        float(value)
        for value in _fg_value(0, "PixelMeasuresSequence", "PixelSpacing")
    )
    positions = [
        tuple(
            float(value)
            for value in _fg_value(frame_index, "PlanePositionSequence", "ImagePositionPatient")
        )
        for frame_index in range(frame_count)
    ]
    base_origin = np.asarray(positions[0], dtype=np.float64)
    projected = [
        float(np.dot(np.asarray(position, dtype=np.float64) - base_origin, normal))
        for position in positions
    ]
    order = sorted(range(frame_count), key=lambda frame_index: (projected[frame_index], frame_index))
    ordered_projected = [projected[index] for index in order]
    diffs = [
        ordered_projected[index + 1] - ordered_projected[index]
        for index in range(len(ordered_projected) - 1)
    ]
    positive_diffs = [diff for diff in diffs if diff > 1e-6]
    spacing_z = float(np.median(np.asarray(positive_diffs, dtype=np.float64))) if positive_diffs else 1.0
    expected_array = np.array(reference_array[order, ...], copy=True)
    expected_origin = tuple(float(value) for value in positions[order[0]])
    expected_spacing = (float(pixel_spacing[1]), float(pixel_spacing[0]), spacing_z)
    expected_direction = tuple(
        float(value)
        for value in (
            row[0], col[0], normal[0],
            row[1], col[1], normal[1],
            row[2], col[2], normal[2],
        )
    )
    return expected_array, expected_origin, expected_spacing, expected_direction


def _load_vtk_numpy_helpers():
    try:
        from vtkmodules.util.numpy_support import vtk_to_numpy
    except ImportError:
        pytest.importorskip("vtk")
        from vtk.util.numpy_support import vtk_to_numpy  # type: ignore
    return vtk_to_numpy


def _vtk_direction(image: object) -> tuple[float, ...]:
    if not hasattr(image, "GetDirectionMatrix"):
        return (
            1.0,
            0.0,
            0.0,
            0.0,
            1.0,
            0.0,
            0.0,
            0.0,
            1.0,
        )

    matrix = image.GetDirectionMatrix()
    if matrix is None:
        return (
            1.0,
            0.0,
            0.0,
            0.0,
            1.0,
            0.0,
            0.0,
            0.0,
            1.0,
        )

    return tuple(
        float(matrix.GetElement(row, col))
        for row in range(3)
        for col in range(3)
    )


def _vtk_array_from_image(image: object, shape: tuple[int, ...]) -> np.ndarray:
    vtk_to_numpy = _load_vtk_numpy_helpers()
    scalars = image.GetPointData().GetScalars()
    assert scalars is not None
    flat = vtk_to_numpy(scalars)
    return np.array(flat.reshape(shape), copy=True)


def test_multiframe_simpleitk_bridge_matches_ct0001_reference() -> None:
    from dicomsdl.simpleitk_bridge import read_series_volume, to_simpleitk_image

    # Chosen from the NEMA CT multiframe ImageFeatures.xml table because it is
    # a clean VOLUME case with 54 frames and consistent per-frame geometry.
    path = _require_multiframe_file("CT/DISCIMG/IMAGES/CT0001")
    sitk, reference_image, reference_array = _load_simpleitk_reference(path)
    expected_array, expected_origin, expected_spacing, expected_direction = _canonical_multiframe_reference(
        path,
        reference_array,
    )

    volume = read_series_volume(path, to_modality_value=True)
    bridge_image = to_simpleitk_image(volume)
    bridge_array = np.array(sitk.GetArrayViewFromImage(bridge_image), copy=True)

    assert volume.source_paths == (path.resolve(),)
    assert volume.array.dtype == np.dtype(np.int16)
    assert bridge_array.dtype == np.dtype(np.int16)
    assert volume.rescale_applied is True
    assert volume.unique_slope_count == 1
    assert tuple(int(value) for value in bridge_image.GetSize()) == (512, 512, 54)

    np.testing.assert_allclose(volume.array, expected_array, rtol=VOXEL_RTOL, atol=VOXEL_ATOL)
    np.testing.assert_allclose(bridge_array, expected_array, rtol=VOXEL_RTOL, atol=VOXEL_ATOL)
    np.testing.assert_allclose(
        bridge_image.GetSpacing(),
        expected_spacing,
        rtol=0.0,
        atol=GEOM_ATOL,
    )
    np.testing.assert_allclose(
        bridge_image.GetOrigin(),
        expected_origin,
        rtol=0.0,
        atol=GEOM_ATOL,
    )
    np.testing.assert_allclose(
        bridge_image.GetDirection(),
        expected_direction,
        rtol=0.0,
        atol=GEOM_ATOL,
    )


def test_multiframe_vtk_bridge_preserves_ct0001_volume() -> None:
    from dicomsdl.vtk_bridge import read_series_volume, to_vtk_image_data

    path = _require_multiframe_file("CT/DISCIMG/IMAGES/CT0001")
    volume = read_series_volume(path, to_modality_value=True)
    image = to_vtk_image_data(volume)
    array = _vtk_array_from_image(image, volume.array.shape)

    assert tuple(int(value) for value in image.GetDimensions()) == (512, 512, 54)
    assert array.dtype == np.dtype(np.int16)
    np.testing.assert_array_equal(array, volume.array)
    np.testing.assert_allclose(image.GetSpacing(), volume.spacing, rtol=0.0, atol=GEOM_ATOL)
    np.testing.assert_allclose(image.GetOrigin(), volume.origin, rtol=0.0, atol=GEOM_ATOL)
    np.testing.assert_allclose(_vtk_direction(image), volume.direction, rtol=0.0, atol=GEOM_ATOL)


def test_multiframe_directory_with_single_file_resolves_to_same_volume(tmp_path: Path) -> None:
    from dicomsdl.simpleitk_bridge import read_series_volume

    path = _require_multiframe_file("CT/DISCIMG/IMAGES/CT0001")
    single_file_dir = tmp_path / "single-file-multiframe"
    single_file_dir.mkdir()
    copied = single_file_dir / path.name
    shutil.copy2(path, copied)

    from_file = read_series_volume(path, to_modality_value=True)
    from_dir = read_series_volume(single_file_dir, to_modality_value=True)

    assert len(from_dir.source_paths) == 1
    assert from_dir.source_paths[0] == copied.resolve()
    np.testing.assert_array_equal(from_dir.array, from_file.array)
    np.testing.assert_allclose(from_dir.spacing, from_file.spacing, rtol=0.0, atol=GEOM_ATOL)
    np.testing.assert_allclose(from_dir.origin, from_file.origin, rtol=0.0, atol=GEOM_ATOL)
    np.testing.assert_allclose(from_dir.direction, from_file.direction, rtol=0.0, atol=GEOM_ATOL)


def test_non_stack_multiframe_localizer_is_rejected() -> None:
    from dicomsdl.simpleitk_bridge import read_series_volume

    path = _require_multiframe_file("MR/DISCIMG/IMAGES/LOCKNEE")

    with pytest.raises(NotImplementedError, match="stack-like multiframe volumes"):
        read_series_volume(path, to_modality_value=True)
