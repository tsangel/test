from __future__ import annotations

from pathlib import Path

import numpy as np
import pytest


REPO_ROOT = Path(__file__).resolve().parents[2]
WG04_ROOT = REPO_ROOT.parent / "sample" / "nema" / "WG04" / "IMAGES" / "REF"
GEOM_ATOL = 1e-4
VOXEL_RTOL = 1e-5
VOXEL_ATOL = 1e-3

WG04_CASES = (
    pytest.param("CT1_UNC", np.int16, True, id="ct1"),
    pytest.param("MR1_UNC", np.int16, False, id="mr1"),
    pytest.param("XA1_UNC", np.uint16, False, id="xa1"),
    pytest.param("US1_UNC", np.uint8, False, id="us1-rgb"),
)


def _require_wg04_file(name: str) -> Path:
    if not WG04_ROOT.is_dir():
        pytest.skip(f"WG04 sample directory is not available: {WG04_ROOT}")
    path = WG04_ROOT / name
    if not path.is_file():
        pytest.skip(f"WG04 sample is not available: {path}")
    return path


def _load_simpleitk_reference(path: Path):
    sitk = pytest.importorskip("SimpleITK")
    image = sitk.ReadImage(str(path))
    array = np.array(sitk.GetArrayViewFromImage(image), copy=True)
    return image, array


def _load_vtk_reference(path: Path):
    try:
        from vtkmodules.vtkIOImage import vtkDICOMImageReader
        from vtkmodules.util.numpy_support import vtk_to_numpy
    except ImportError:
        pytest.importorskip("vtk")
        import vtk  # type: ignore

        vtkDICOMImageReader = vtk.vtkDICOMImageReader
        from vtk.util.numpy_support import vtk_to_numpy  # type: ignore

    reader = vtkDICOMImageReader()
    reader.SetFileName(str(path))
    reader.Update()
    image = reader.GetOutput()
    scalars = image.GetPointData().GetScalars()
    if scalars is None:
        raise RuntimeError(f"VTK did not produce scalar data for {path}")

    dims = tuple(int(value) for value in image.GetDimensions())
    components = int(scalars.GetNumberOfComponents())
    flat = vtk_to_numpy(scalars)
    if components == 1:
        array = np.array(flat.reshape(dims[2], dims[1], dims[0]), copy=True)
    else:
        array = np.array(flat.reshape(dims[2], dims[1], dims[0], components), copy=True)
    return image, array


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


def _flip_vtk_reference_y(array: np.ndarray) -> np.ndarray:
    return array[:, ::-1, ...]


@pytest.mark.parametrize("sample_name, expected_dtype, expected_rescale_applied", WG04_CASES)
def test_wg04_simpleitk_bridge_matches_reference(
    sample_name: str,
    expected_dtype: np.dtype,
    expected_rescale_applied: bool,
) -> None:
    from dicomsdl.simpleitk_bridge import read_series_volume, to_simpleitk_image

    path = _require_wg04_file(sample_name)
    reference_image, reference_array = _load_simpleitk_reference(path)

    volume = read_series_volume(path, to_modality_value=True)
    bridge_image = to_simpleitk_image(volume)
    bridge_array = np.array(
        pytest.importorskip("SimpleITK").GetArrayViewFromImage(bridge_image),
        copy=True,
    )

    assert volume.rescale_applied is expected_rescale_applied
    assert volume.array.dtype == np.dtype(expected_dtype)
    assert bridge_array.dtype == np.dtype(expected_dtype)
    assert tuple(int(value) for value in bridge_image.GetSize()) == tuple(
        int(value) for value in reference_image.GetSize()
    )

    np.testing.assert_allclose(volume.array, reference_array, rtol=VOXEL_RTOL, atol=VOXEL_ATOL)
    np.testing.assert_allclose(bridge_array, reference_array, rtol=VOXEL_RTOL, atol=VOXEL_ATOL)
    np.testing.assert_allclose(
        bridge_image.GetSpacing(),
        reference_image.GetSpacing(),
        rtol=0.0,
        atol=GEOM_ATOL,
    )
    np.testing.assert_allclose(
        bridge_image.GetOrigin(),
        reference_image.GetOrigin(),
        rtol=0.0,
        atol=GEOM_ATOL,
    )
    np.testing.assert_allclose(
        bridge_image.GetDirection(),
        reference_image.GetDirection(),
        rtol=0.0,
        atol=GEOM_ATOL,
    )


@pytest.mark.parametrize("sample_name, expected_dtype, _expected_rescale_applied", WG04_CASES)
def test_wg04_vtk_bridge_matches_reference_pixels(
    sample_name: str,
    expected_dtype: np.dtype,
    _expected_rescale_applied: bool,
) -> None:
    from dicomsdl.vtk_bridge import read_series_volume, to_vtk_image_data

    path = _require_wg04_file(sample_name)
    reference_image, reference_array = _load_vtk_reference(path)

    volume = read_series_volume(path, to_modality_value=True)
    bridge_image = to_vtk_image_data(volume)
    bridge_scalars = bridge_image.GetPointData().GetScalars()
    assert bridge_scalars is not None

    try:
        from vtkmodules.util.numpy_support import vtk_to_numpy
    except ImportError:
        from vtk.util.numpy_support import vtk_to_numpy  # type: ignore

    bridge_dims = tuple(int(value) for value in bridge_image.GetDimensions())
    bridge_components = int(bridge_scalars.GetNumberOfComponents())
    bridge_flat = vtk_to_numpy(bridge_scalars)
    if bridge_components == 1:
        bridge_array = np.array(
            bridge_flat.reshape(bridge_dims[2], bridge_dims[1], bridge_dims[0]),
            copy=True,
        )
    else:
        bridge_array = np.array(
            bridge_flat.reshape(bridge_dims[2], bridge_dims[1], bridge_dims[0], bridge_components),
            copy=True,
        )

    assert volume.array.dtype == np.dtype(expected_dtype)
    assert bridge_array.dtype == np.dtype(expected_dtype)
    np.testing.assert_array_equal(bridge_array, volume.array)
    np.testing.assert_allclose(
        bridge_image.GetSpacing(),
        volume.spacing,
        rtol=0.0,
        atol=GEOM_ATOL,
    )
    np.testing.assert_allclose(
        _vtk_direction(bridge_image),
        volume.direction,
        rtol=0.0,
        atol=GEOM_ATOL,
    )

    np.testing.assert_allclose(
        bridge_array,
        _flip_vtk_reference_y(reference_array),
        rtol=VOXEL_RTOL,
        atol=VOXEL_ATOL,
    )
    np.testing.assert_allclose(
        bridge_image.GetSpacing()[:2],
        reference_image.GetSpacing()[:2],
        rtol=0.0,
        atol=GEOM_ATOL,
    )
