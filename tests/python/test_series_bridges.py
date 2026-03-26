from __future__ import annotations

from pathlib import Path

import numpy as np
import pytest


REPO_ROOT = Path(__file__).resolve().parents[2]
SAMPLE_PT_ROOT = REPO_ROOT.parent / "sample" / "PT"
VOXEL_RTOL = 1e-5
VOXEL_ATOL = 1e-3
GEOM_ATOL = 1e-4

SERIES_CASES = (
    pytest.param("00003 CT Torso", np.int16, True, id="ct-torso"),
    pytest.param("00003 CT_Torso_AX", np.int16, True, id="ct-torso-ax"),
    pytest.param("00010 AXL 3D BRAVO CE", np.int16, False, id="mr-bravo"),
    pytest.param("00013 Torso PET AC OSEM", np.float32, True, id="pet-osem"),
    pytest.param("01202 sT1 mDIXON AX GD W", np.uint16, False, id="mr-mdixon"),
)


def _require_series_dir(series_name: str) -> Path:
    if not SAMPLE_PT_ROOT.is_dir():
        pytest.skip(f"Sample directory is not available: {SAMPLE_PT_ROOT}")

    series_dir = SAMPLE_PT_ROOT / series_name
    if not series_dir.is_dir():
        pytest.skip(f"Sample series is not available: {series_dir}")

    return series_dir


def _load_simpleitk_reference(series_dir: Path):
    sitk = pytest.importorskip("SimpleITK")

    series_ids = list(sitk.ImageSeriesReader.GetGDCMSeriesIDs(str(series_dir)) or [])
    if series_ids:
        file_names = list(
            sitk.ImageSeriesReader.GetGDCMSeriesFileNames(str(series_dir), series_ids[0])
        )
    else:
        file_names = list(sitk.ImageSeriesReader.GetGDCMSeriesFileNames(str(series_dir)))
    if not file_names:
        raise RuntimeError(f"SimpleITK could not enumerate DICOM files under {series_dir}")

    reader = sitk.ImageSeriesReader()
    reader.SetFileNames(file_names)
    reader.MetaDataDictionaryArrayUpdateOn()
    reader.LoadPrivateTagsOn()
    image = reader.Execute()
    array = np.array(sitk.GetArrayViewFromImage(image), copy=True)
    return image, array, tuple(Path(name).resolve() for name in file_names)


def _series_geometry_from_metadata(source_paths: tuple[Path, ...]):
    pydicom = pytest.importorskip("pydicom")
    if not source_paths:
        raise RuntimeError("No source paths were provided")

    def _unit(vec: np.ndarray) -> np.ndarray:
        return vec / np.linalg.norm(vec)

    infos = []
    for path in source_paths:
        ds = pydicom.dcmread(str(path), stop_before_pixels=True)
        ipp = np.array([float(value) for value in ds.ImagePositionPatient], dtype=np.float64)
        iop = [float(value) for value in ds.ImageOrientationPatient]
        row = _unit(np.asarray(iop[:3], dtype=np.float64))
        col = _unit(np.asarray(iop[3:], dtype=np.float64))
        normal = _unit(np.cross(row, col))
        pixel_spacing = tuple(float(value) for value in ds.PixelSpacing)
        infos.append((ipp, row, col, normal, pixel_spacing))

    first_origin, row, col, normal, pixel_spacing = infos[0]
    projected = [
        float(np.dot(origin - first_origin, normal))
        for origin, _row, _col, _normal, _spacing in infos
    ]
    diffs = [
        projected[index + 1] - projected[index]
        for index in range(len(projected) - 1)
    ]
    positive_diffs = [diff for diff in diffs if diff > 1e-6]
    spacing_z = float(np.median(np.asarray(positive_diffs, dtype=np.float64))) if positive_diffs else 1.0
    spacing = (float(pixel_spacing[1]), float(pixel_spacing[0]), spacing_z)
    direction = tuple(
        float(value)
        for value in (
            row[0], col[0], normal[0],
            row[1], col[1], normal[1],
            row[2], col[2], normal[2],
        )
    )
    return (
        tuple(float(value) for value in first_origin),
        spacing,
        direction,
    )


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


@pytest.mark.parametrize("series_name, expected_dtype, expected_rescale_applied", SERIES_CASES)
def test_read_series_volume_matches_simpleitk_reference(
    series_name: str,
    expected_dtype: np.dtype,
    expected_rescale_applied: bool,
) -> None:
    from dicomsdl.simpleitk_bridge import read_series_volume, to_simpleitk_image

    series_dir = _require_series_dir(series_name)
    reference_image, reference_array, reference_paths = _load_simpleitk_reference(series_dir)

    volume = read_series_volume(series_dir, to_modality_value=True)
    bridge_image = to_simpleitk_image(volume)
    bridge_array = np.array(
        pytest.importorskip("SimpleITK").GetArrayViewFromImage(bridge_image),
        copy=True,
    )
    reference_lookup = {path: index for index, path in enumerate(reference_paths)}
    expected_array = np.array(
        reference_array[[reference_lookup[path] for path in volume.source_paths], ...],
        copy=True,
    )
    expected_origin, expected_spacing, expected_direction = _series_geometry_from_metadata(volume.source_paths)

    assert volume.modality
    assert volume.rescale_applied is expected_rescale_applied
    assert 1 <= len(volume.source_paths) <= len(list(series_dir.glob("*.dcm")))
    assert volume.array.dtype == np.dtype(expected_dtype)
    assert bridge_array.dtype == np.dtype(expected_dtype)

    np.testing.assert_allclose(
        volume.array,
        expected_array,
        rtol=VOXEL_RTOL,
        atol=VOXEL_ATOL,
    )
    np.testing.assert_allclose(
        bridge_array,
        expected_array,
        rtol=VOXEL_RTOL,
        atol=VOXEL_ATOL,
    )
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


@pytest.mark.parametrize("series_name, expected_dtype, _expected_rescale_applied", SERIES_CASES)
def test_to_vtk_image_data_preserves_series_volume(
    series_name: str,
    expected_dtype: np.dtype,
    _expected_rescale_applied: bool,
) -> None:
    from dicomsdl.vtk_bridge import read_series_volume, to_vtk_image_data

    series_dir = _require_series_dir(series_name)

    volume = read_series_volume(series_dir, to_modality_value=True)
    image = to_vtk_image_data(volume)
    array = _vtk_array_from_image(image, volume.array.shape)

    assert volume.array.dtype == np.dtype(expected_dtype)
    assert array.dtype == np.dtype(expected_dtype)
    np.testing.assert_array_equal(array, volume.array)
    np.testing.assert_allclose(image.GetSpacing(), volume.spacing, rtol=0.0, atol=GEOM_ATOL)
    np.testing.assert_allclose(image.GetOrigin(), volume.origin, rtol=0.0, atol=GEOM_ATOL)
    np.testing.assert_allclose(
        _vtk_direction(image),
        volume.direction,
        rtol=0.0,
        atol=GEOM_ATOL,
    )


def test_to_vtk_image_data_copy_flag_controls_buffer_sharing() -> None:
    from dicomsdl.vtk_bridge import read_series_volume, to_vtk_image_data

    series_dir = _require_series_dir("00003 CT Torso")
    volume = read_series_volume(series_dir, to_modality_value=True)
    shared_image = to_vtk_image_data(volume)
    copied_image = to_vtk_image_data(volume, copy=True)
    original = np.array(volume.array, copy=True)

    updated_value = np.array(original.flat[0] + 1, dtype=original.dtype).item()
    volume.array.flat[0] = updated_value

    shared_array = _vtk_array_from_image(shared_image, volume.array.shape)
    copied_array = _vtk_array_from_image(copied_image, volume.array.shape)

    assert shared_array.flat[0] == updated_value
    assert copied_array.flat[0] == original.flat[0]
    np.testing.assert_array_equal(copied_array, original)


def test_processed_series_excludes_orientation_outlier_before_canonical_sort() -> None:
    from dicomsdl.simpleitk_bridge import read_series_volume

    pydicom = pytest.importorskip("pydicom")
    series_dir = _require_series_dir("01000 Processed Images")
    volume = read_series_volume(series_dir, to_modality_value=True)

    instance_numbers = [
        int(pydicom.dcmread(str(path), stop_before_pixels=True).InstanceNumber)
        for path in volume.source_paths
    ]

    assert len(volume.source_paths) == 49
    assert 1 not in instance_numbers
    assert instance_numbers[0] == 50
    assert instance_numbers[-1] == 2
    np.testing.assert_allclose(
        volume.origin,
        (56.483944, -153.299057, 134.722321),
        rtol=0.0,
        atol=GEOM_ATOL,
    )
    np.testing.assert_allclose(volume.spacing[2], 3.020001, rtol=0.0, atol=1e-3)
