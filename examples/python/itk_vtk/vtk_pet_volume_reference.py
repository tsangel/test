#!/usr/bin/env python3
"""Reference PET volume reader using VTK's built-in DICOM reader."""

from __future__ import annotations

import argparse
import sys

from _pet_series_reference_common import (
    add_series_selection_args,
    count_dicom_files,
    resolve_series_dir,
)


def _load_vtk():
    try:
        from vtkmodules.util.numpy_support import vtk_to_numpy
        from vtkmodules.vtkIOImage import vtkDICOMImageReader
    except ImportError:
        try:
            import vtk  # type: ignore
            from vtk.util.numpy_support import vtk_to_numpy  # type: ignore
        except ImportError as exc:
            raise RuntimeError(
                "VTK is not installed. Install it with: pip install vtk"
            ) from exc
        vtk_dicom_image_reader = vtk.vtkDICOMImageReader
    else:
        vtk_dicom_image_reader = vtkDICOMImageReader
    return vtk_dicom_image_reader, vtk_to_numpy


def _direction_tuple(image: object) -> tuple[float, ...] | None:
    if not hasattr(image, "GetDirectionMatrix"):
        return None
    matrix = image.GetDirectionMatrix()
    if matrix is None:
        return None
    return tuple(matrix.GetElement(row, col) for row in range(3) for col in range(3))


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Read a PET DICOM volume with vtkDICOMImageReader and print a compact summary.",
    )
    add_series_selection_args(parser)
    args = parser.parse_args(argv)

    try:
        vtk_dicom_image_reader, vtk_to_numpy = _load_vtk()
        series_dir, discovered = resolve_series_dir(args.path, args.series_index, args.series_name)
    except (FileNotFoundError, IndexError, NotADirectoryError, RuntimeError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    reader = vtk_dicom_image_reader()
    reader.SetDirectoryName(str(series_dir))
    reader.Update()

    image = reader.GetOutput()
    dims = tuple(image.GetDimensions())
    scalars = image.GetPointData().GetScalars()
    if scalars is None:
        print(f"error: VTK did not produce scalar pixel data for {series_dir}", file=sys.stderr)
        return 2

    components = scalars.GetNumberOfComponents()
    flat = vtk_to_numpy(scalars)
    if components == 1:
        array_view = flat.reshape(dims[2], dims[1], dims[0])
    else:
        array_view = flat.reshape(dims[2], dims[1], dims[0], components)

    print("Framework: VTK")
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

    patient_name = reader.GetPatientName()
    if patient_name:
        print(f"PatientName: {patient_name}")
    study_id = reader.GetStudyID()
    if study_id:
        print(f"StudyID: {study_id}")
    transfer_syntax = reader.GetTransferSyntaxUID()
    if transfer_syntax:
        print(f"TransferSyntaxUID: {transfer_syntax}")
    print(f"RescaleSlope: {reader.GetRescaleSlope()}")
    print(f"RescaleOffset: {reader.GetRescaleOffset()}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
