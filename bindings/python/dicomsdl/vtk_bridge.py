from __future__ import annotations

from pathlib import Path

import numpy as np

from ._bridge_common import SeriesVolume, read_series_volume


def _load_vtk():
    try:
        from vtkmodules.vtkCommonDataModel import vtkImageData
        from vtkmodules.util.numpy_support import get_vtk_array_type, numpy_to_vtk
    except ImportError:
        try:
            import vtk  # type: ignore
            vtk_image_data = vtk.vtkImageData
            from vtk.util.numpy_support import get_vtk_array_type, numpy_to_vtk  # type: ignore
        except ImportError as exc:
            raise RuntimeError(
                "VTK is not installed. Install it with: pip install vtk"
            ) from exc
    else:
        vtk_image_data = vtkImageData
    return vtk_image_data, numpy_to_vtk, get_vtk_array_type


def _vtk_array_layout(array: np.ndarray) -> tuple[int, int, int, int]:
    if array.ndim == 3:
        depth, height, width = array.shape
        components = 1
    elif array.ndim == 4:
        depth, height, width, components = array.shape
    else:
        raise ValueError(
            f"Expected a scalar 3D array or vector 4D array, got shape {array.shape}"
        )

    return depth, height, width, components


def to_vtk_image_data(volume: SeriesVolume, *, copy: bool = False):
    vtk_image_data, numpy_to_vtk, get_vtk_array_type = _load_vtk()
    depth, height, width, components = _vtk_array_layout(volume.array)
    if components == 1:
        flat_array = np.ascontiguousarray(volume.array.reshape(-1), dtype=volume.array.dtype)
    elif volume.array.ndim == 4:
        flat_array = np.ascontiguousarray(
            volume.array.reshape(-1, components),
            dtype=volume.array.dtype,
        )
    else:
        raise AssertionError("Unreachable: _vtk_array_layout validates dimensionality")

    scalars = numpy_to_vtk(
        flat_array,
        deep=bool(copy),
        array_type=get_vtk_array_type(volume.array.dtype),
    )

    image = vtk_image_data()
    image.SetDimensions(width, height, depth)
    image.SetSpacing(*volume.spacing)
    image.SetOrigin(*volume.origin)
    image.SetDirectionMatrix(volume.direction)
    image.GetPointData().SetScalars(scalars)
    if not copy:
        # VTK does not own the NumPy buffer in the zero-copy path, so keep
        # the Python-side objects alive for as long as the image lives.
        image._dicomsdl_volume = volume
        image._dicomsdl_flat_array = flat_array
        image._dicomsdl_scalars = scalars
    return image


def read_series_image_data(
    path: str | Path,
    *,
    to_modality_value: bool = True,
    copy: bool = False,
):
    return to_vtk_image_data(
        read_series_volume(path, to_modality_value=to_modality_value),
        copy=copy,
    )


__all__ = (
    "SeriesVolume",
    "read_series_volume",
    "to_vtk_image_data",
    "read_series_image_data",
)
