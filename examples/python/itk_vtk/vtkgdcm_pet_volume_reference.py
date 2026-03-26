#!/usr/bin/env python3
"""Reference PET volume reader using vtkgdcm's vtkGDCMImageReader."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from _pet_series_reference_common import (
    add_series_selection_args,
    count_dicom_files,
    resolve_series_dir,
)


def _load_vtk_and_vtkgdcm():
    try:
        from vtkmodules.util.numpy_support import vtk_to_numpy
        from vtkmodules.vtkCommonCore import vtkStringArray
    except ImportError:
        try:
            import vtk  # type: ignore
            from vtk.util.numpy_support import vtk_to_numpy  # type: ignore
        except ImportError as exc:
            raise RuntimeError(
                "VTK is not installed. Install it with: pip install vtk"
            ) from exc
        vtk_string_array = vtk.vtkStringArray
    else:
        vtk_string_array = vtkStringArray

    try:
        import vtkgdcm  # type: ignore
    except ImportError as exc:
        raise RuntimeError(
            "vtkgdcm is not installed in this Python environment. "
            "Use the dicom-bench conda env or install a vtkgdcm build first."
        ) from exc

    return vtkgdcm, vtk_string_array, vtk_to_numpy


def _direction_tuple(image: object) -> tuple[float, ...] | None:
    if not hasattr(image, "GetDirectionMatrix"):
        return None
    matrix = image.GetDirectionMatrix()
    if matrix is None:
        return None
    return tuple(matrix.GetElement(row, col) for row in range(3) for col in range(3))


def _series_file_names(series_dir: Path) -> list[str]:
    file_names = [
        str(child)
        for child in sorted(series_dir.iterdir())
        if child.is_file() and child.suffix.lower() == ".dcm"
    ]
    if not file_names:
        raise RuntimeError(f"No DICOM files found under {series_dir}")
    return file_names


def _configure_reader(reader: object, series_dir: Path, vtk_string_array: type) -> None:
    if hasattr(reader, "SetDirectoryName"):
        reader.SetDirectoryName(str(series_dir))
        return

    if hasattr(reader, "SetFileNames"):
        file_names = vtk_string_array()
        for file_name in _series_file_names(series_dir):
            file_names.InsertNextValue(file_name)
        reader.SetFileNames(file_names)
        return

    raise RuntimeError(
        "vtkGDCMImageReader does not support directory or filename-array input in this build"
    )


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Read a PET DICOM volume with vtkgdcm and print a compact summary.",
    )
    add_series_selection_args(parser)
    args = parser.parse_args(argv)

    try:
        vtkgdcm, vtk_string_array, vtk_to_numpy = _load_vtk_and_vtkgdcm()
        series_dir, discovered = resolve_series_dir(args.path, args.series_index, args.series_name)
    except (FileNotFoundError, IndexError, NotADirectoryError, RuntimeError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    reader = vtkgdcm.vtkGDCMImageReader()
    try:
        _configure_reader(reader, series_dir, vtk_string_array)
    except RuntimeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    reader.Update()

    image = reader.GetOutput()
    dims = tuple(int(value) for value in image.GetDimensions())
    scalars = image.GetPointData().GetScalars()
    if scalars is None:
        print(f"error: vtkgdcm did not produce scalar pixel data for {series_dir}", file=sys.stderr)
        return 2

    components = int(scalars.GetNumberOfComponents())
    flat = vtk_to_numpy(scalars)
    if components == 1:
        array_view = flat.reshape(dims[2], dims[1], dims[0])
    else:
        array_view = flat.reshape(dims[2], dims[1], dims[0], components)

    print("Framework: vtkgdcm")
    print(f"Series directory: {series_dir}")
    print(f"Discovered series dirs: {len(discovered)}")
    print(f"DICOM files: {count_dicom_files(series_dir)}")
    print(f"Image dimensions (x, y, z): {dims}")
    print(f"Array shape: {tuple(array_view.shape)}")
    print(f"Scalar type: {scalars.GetDataTypeAsString()}")
    print(f"Scalar components: {components}")
    print(f"Spacing: {tuple(image.GetSpacing())}")
    print(f"Origin: {tuple(image.GetOrigin())}")
    direction = _direction_tuple(image)
    if direction is not None:
        print(f"Direction: {direction}")
    print(f"Value range: min={array_view.min()} max={array_view.max()}")

    patient_name = getattr(reader, "GetPatientName", lambda: "")()
    if patient_name:
        print(f"PatientName: {patient_name}")
    if hasattr(reader, "GetShift"):
        print(f"Shift: {reader.GetShift()}")
    if hasattr(reader, "GetScale"):
        print(f"Scale: {reader.GetScale()}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
