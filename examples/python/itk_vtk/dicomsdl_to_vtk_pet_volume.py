#!/usr/bin/env python3
"""Build a PET volume with DicomSDL, then hand it to VTK."""

from __future__ import annotations

import argparse
import sys

from _repo_dicomsdl_import import configure_repo_dicomsdl_import
from _pet_series_reference_common import add_series_selection_args, count_dicom_files, resolve_series_dir

configure_repo_dicomsdl_import()

from dicomsdl.vtk_bridge import read_series_volume, to_vtk_image_data


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Read a PET DICOM series with DicomSDL and convert it into a VTK image.",
    )
    add_series_selection_args(parser)
    parser.add_argument(
        "--stored-value",
        action="store_true",
        help="Keep stored pixel values instead of applying per-slice PET rescale to modality values.",
    )
    args = parser.parse_args(argv)

    try:
        try:
            from vtkmodules.util.numpy_support import vtk_to_numpy
        except ImportError:
            from vtk.util.numpy_support import vtk_to_numpy  # type: ignore
        series_dir, discovered = resolve_series_dir(args.path, args.series_index, args.series_name)
        volume = read_series_volume(series_dir, to_modality_value=not args.stored_value)
        image = to_vtk_image_data(volume)
    except (FileNotFoundError, IndexError, NotADirectoryError, RuntimeError, ValueError, NotImplementedError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    scalars = image.GetPointData().GetScalars()
    flat = vtk_to_numpy(scalars)
    array_view = flat.reshape(volume.array.shape)

    print("Source reader: DicomSDL")
    print("Target object: vtkImageData")
    print("Bridge module: dicomsdl.vtk_bridge")
    print(f"Series directory: {series_dir}")
    print(f"Discovered series dirs: {len(discovered)}")
    print(f"DICOM files: {count_dicom_files(series_dir)}")
    print(f"Image dimensions (x, y, z): {tuple(image.GetDimensions())}")
    print(f"Array shape: {tuple(array_view.shape)}")
    print(f"Scalar type: {scalars.GetDataTypeAsString()}")
    print(f"Spacing: {tuple(image.GetSpacing())}")
    print(f"Origin: {tuple(image.GetOrigin())}")
    if hasattr(image, "GetDirectionMatrix"):
        direction_matrix = image.GetDirectionMatrix()
        direction = tuple(direction_matrix.GetElement(row, col) for row in range(3) for col in range(3))
        print(f"Direction: {direction}")
    print(f"Value range: min={array_view.min()} max={array_view.max()}")
    print(f"ModalityValueApplied: {volume.rescale_applied}")
    print(f"UniqueRescalePairs: {volume.unique_slope_count}")
    if volume.modality:
        print(f"Modality: {volume.modality}")
    if volume.units:
        print(f"Units: {volume.units}")
    if volume.series_description:
        print(f"SeriesDescription: {volume.series_description}")
    if volume.patient_name:
        print(f"PatientName: {volume.patient_name}")
    if volume.study_id:
        print(f"StudyID: {volume.study_id}")
    if volume.transfer_syntax_uid:
        print(f"TransferSyntaxUID: {volume.transfer_syntax_uid}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
