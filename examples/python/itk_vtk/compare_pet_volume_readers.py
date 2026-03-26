#!/usr/bin/env python3
"""Compare PET volume outputs across reference readers and DicomSDL bridges."""

from __future__ import annotations

import argparse
import math
import sys
from dataclasses import dataclass
from pathlib import Path

import numpy as np

_THIS_DIR = Path(__file__).resolve().parent
if str(_THIS_DIR) not in sys.path:
    sys.path.insert(0, str(_THIS_DIR))

from _repo_dicomsdl_import import configure_repo_dicomsdl_import
from _pet_series_reference_common import add_series_selection_args, count_dicom_files, resolve_series_dir

configure_repo_dicomsdl_import()

from dicomsdl.simpleitk_bridge import read_series_volume, to_simpleitk_image
from dicomsdl.vtk_bridge import to_vtk_image_data


@dataclass(slots=True)
class LoadedVolume:
    label: str
    array: np.ndarray
    spacing: tuple[float, float, float]
    origin: tuple[float, float, float]
    direction: tuple[float, ...]


def _load_simpleitk():
    try:
        import SimpleITK as sitk
    except ImportError as exc:
        raise RuntimeError(
            "SimpleITK is not installed. Install it with: pip install SimpleITK"
        ) from exc
    return sitk


def _load_vtk():
    try:
        from vtkmodules.util.numpy_support import get_vtk_array_type, vtk_to_numpy
        from vtkmodules.vtkIOImage import vtkDICOMImageReader, vtkImageImport
    except ImportError:
        try:
            import vtk  # type: ignore
            from vtk.util.numpy_support import get_vtk_array_type, vtk_to_numpy  # type: ignore
        except ImportError as exc:
            raise RuntimeError(
                "VTK is not installed. Install it with: pip install vtk"
            ) from exc
        vtk_dicom_image_reader = vtk.vtkDICOMImageReader
        vtk_image_import = vtk.vtkImageImport
    else:
        vtk_dicom_image_reader = vtkDICOMImageReader
        vtk_image_import = vtkImageImport
    return vtk_dicom_image_reader, vtk_image_import, get_vtk_array_type, vtk_to_numpy


def _simpleitk_reference(series_dir: Path) -> LoadedVolume:
    sitk = _load_simpleitk()
    series_ids = list(sitk.ImageSeriesReader.GetGDCMSeriesIDs(str(series_dir)) or [])
    if series_ids:
        file_names = list(sitk.ImageSeriesReader.GetGDCMSeriesFileNames(str(series_dir), series_ids[0]))
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
    return LoadedVolume(
        label="SimpleITK reference",
        array=array,
        spacing=tuple(float(value) for value in image.GetSpacing()),
        origin=tuple(float(value) for value in image.GetOrigin()),
        direction=tuple(float(value) for value in image.GetDirection()),
    )


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
    return tuple(float(matrix.GetElement(row, col)) for row in range(3) for col in range(3))


def _vtk_reference(series_dir: Path) -> LoadedVolume:
    vtk_dicom_image_reader, _, _, vtk_to_numpy = _load_vtk()
    reader = vtk_dicom_image_reader()
    reader.SetDirectoryName(str(series_dir))
    reader.Update()
    image = reader.GetOutput()
    dims = tuple(int(value) for value in image.GetDimensions())
    scalars = image.GetPointData().GetScalars()
    if scalars is None:
        raise RuntimeError(f"VTK did not produce scalar data for {series_dir}")
    flat = vtk_to_numpy(scalars)
    array = np.array(flat.reshape(dims[2], dims[1], dims[0]), copy=True)
    return LoadedVolume(
        label="VTK reference",
        array=array,
        spacing=tuple(float(value) for value in image.GetSpacing()),
        origin=tuple(float(value) for value in image.GetOrigin()),
        direction=_vtk_direction(image),
    )


def _dicomsdl_to_simpleitk(series_dir: Path) -> LoadedVolume:
    volume = read_series_volume(series_dir, to_modality_value=True)
    image = to_simpleitk_image(volume)
    return LoadedVolume(
        label="DicomSDL -> SimpleITK",
        array=np.array(volume.array, copy=True),
        spacing=tuple(float(value) for value in image.GetSpacing()),
        origin=tuple(float(value) for value in image.GetOrigin()),
        direction=tuple(float(value) for value in image.GetDirection()),
    )


def _dicomsdl_to_vtk(series_dir: Path) -> LoadedVolume:
    volume = read_series_volume(series_dir, to_modality_value=True)
    _, _, _, vtk_to_numpy = _load_vtk()
    image = to_vtk_image_data(volume)
    scalars = image.GetPointData().GetScalars()
    if scalars is None:
        raise RuntimeError("DicomSDL bridge did not produce VTK scalar data")
    flat = vtk_to_numpy(scalars)
    array = np.array(flat.reshape(volume.array.shape), copy=True)
    return LoadedVolume(
        label="DicomSDL -> VTK",
        array=array,
        spacing=tuple(float(value) for value in image.GetSpacing()),
        origin=tuple(float(value) for value in image.GetOrigin()),
        direction=_vtk_direction(image),
    )


def _max_abs_diff(lhs: tuple[float, ...], rhs: tuple[float, ...]) -> float:
    if len(lhs) != len(rhs):
        return math.inf
    return max(abs(a - b) for a, b in zip(lhs, rhs)) if lhs else 0.0


def _print_loaded_summary(volume: LoadedVolume) -> None:
    print(f"{volume.label}:")
    print(f"  shape={tuple(volume.array.shape)} dtype={volume.array.dtype}")
    print(f"  spacing={volume.spacing}")
    print(f"  origin={volume.origin}")
    print(f"  direction={volume.direction}")
    print(f"  range=({float(volume.array.min())}, {float(volume.array.max())})")


def _compare_pair(
    lhs: LoadedVolume,
    rhs: LoadedVolume,
    *,
    rtol: float,
    atol: float,
    geom_atol: float,
) -> None:
    print(f"{lhs.label} vs {rhs.label}:")
    print(f"  shape_match={lhs.array.shape == rhs.array.shape}")
    if lhs.array.shape != rhs.array.shape:
        return

    lhs64 = lhs.array.astype(np.float64, copy=False)
    rhs64 = rhs.array.astype(np.float64, copy=False)
    diff = np.abs(lhs64 - rhs64)
    allclose = bool(np.allclose(lhs64, rhs64, rtol=rtol, atol=atol))
    print(f"  array_allclose={allclose} rtol={rtol} atol={atol}")
    print(f"  max_abs_diff={float(diff.max())}")
    print(f"  mean_abs_diff={float(diff.mean())}")
    if not allclose:
        flip_label, flip_allclose, flip_max_abs, flip_mean_abs = _best_flip_match(
            lhs64,
            rhs64,
            rtol=rtol,
            atol=atol,
        )
        if flip_label is not None:
            print(
                f"  best_axis_flip={flip_label} array_allclose={flip_allclose} "
                f"max_abs_diff={flip_max_abs} mean_abs_diff={flip_mean_abs}"
            )
    print(f"  spacing_allclose={np.allclose(lhs.spacing, rhs.spacing, atol=geom_atol, rtol=0.0)} max_abs_diff={_max_abs_diff(lhs.spacing, rhs.spacing)}")
    print(f"  origin_allclose={np.allclose(lhs.origin, rhs.origin, atol=geom_atol, rtol=0.0)} max_abs_diff={_max_abs_diff(lhs.origin, rhs.origin)}")
    print(f"  direction_allclose={np.allclose(lhs.direction, rhs.direction, atol=geom_atol, rtol=0.0)} max_abs_diff={_max_abs_diff(lhs.direction, rhs.direction)}")


def _best_flip_match(
    lhs: np.ndarray,
    rhs: np.ndarray,
    *,
    rtol: float,
    atol: float,
) -> tuple[str | None, bool, float, float]:
    if lhs.ndim != 3 or rhs.ndim != 3:
        return (None, False, math.inf, math.inf)

    best_label = None
    best_allclose = False
    best_max_abs = math.inf
    best_mean_abs = math.inf
    for flip_z in (False, True):
        for flip_y in (False, True):
            for flip_x in (False, True):
                if not (flip_z or flip_y or flip_x):
                    continue
                candidate = rhs
                labels: list[str] = []
                if flip_z:
                    candidate = candidate[::-1, :, :]
                    labels.append("z")
                if flip_y:
                    candidate = candidate[:, ::-1, :]
                    labels.append("y")
                if flip_x:
                    candidate = candidate[:, :, ::-1]
                    labels.append("x")
                diff = np.abs(lhs - candidate)
                max_abs = float(diff.max())
                mean_abs = float(diff.mean())
                is_allclose = bool(np.allclose(lhs, candidate, rtol=rtol, atol=atol))
                if is_allclose and not best_allclose:
                    best_label = "+".join(labels)
                    best_allclose = True
                    best_max_abs = max_abs
                    best_mean_abs = mean_abs
                    continue
                if is_allclose == best_allclose and max_abs < best_max_abs:
                    best_label = "+".join(labels)
                    best_max_abs = max_abs
                    best_mean_abs = mean_abs
    return (best_label, best_allclose, best_max_abs, best_mean_abs)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Compare PET volume outputs from SimpleITK/VTK reference readers and DicomSDL bridge loaders.",
    )
    add_series_selection_args(parser)
    parser.add_argument("--rtol", type=float, default=1e-5, help="Relative tolerance for voxel comparison")
    parser.add_argument("--atol", type=float, default=1e-3, help="Absolute tolerance for voxel comparison")
    parser.add_argument(
        "--geom-atol",
        type=float,
        default=1e-4,
        help="Absolute tolerance for spacing/origin/direction comparisons",
    )
    args = parser.parse_args(argv)

    try:
        series_dir, discovered = resolve_series_dir(args.path, args.series_index, args.series_name)
        simpleitk_ref = _simpleitk_reference(series_dir)
        vtk_ref = _vtk_reference(series_dir)
        dicomsdl_sitk = _dicomsdl_to_simpleitk(series_dir)
        dicomsdl_vtk = _dicomsdl_to_vtk(series_dir)
    except (FileNotFoundError, IndexError, NotADirectoryError, RuntimeError, ValueError, NotImplementedError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    print(f"Series directory: {series_dir}")
    print(f"Discovered series dirs: {len(discovered)}")
    print(f"DICOM files: {count_dicom_files(series_dir)}")
    print()

    for loaded in (simpleitk_ref, vtk_ref, dicomsdl_sitk, dicomsdl_vtk):
        _print_loaded_summary(loaded)
        print()

    _compare_pair(simpleitk_ref, dicomsdl_sitk, rtol=args.rtol, atol=args.atol, geom_atol=args.geom_atol)
    print()
    _compare_pair(vtk_ref, dicomsdl_vtk, rtol=args.rtol, atol=args.atol, geom_atol=args.geom_atol)
    print()
    _compare_pair(dicomsdl_sitk, dicomsdl_vtk, rtol=args.rtol, atol=args.atol, geom_atol=args.geom_atol)
    print()
    _compare_pair(simpleitk_ref, vtk_ref, rtol=args.rtol, atol=args.atol, geom_atol=args.geom_atol)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
