#!/usr/bin/env python3
"""Build a WG04 single-file image with DicomSDL, then hand it to VTK."""

from __future__ import annotations

import argparse
import sys

from _repo_dicomsdl_import import configure_repo_dicomsdl_import
from _wg04_reference_common import add_sample_selection_args, resolve_sample_file

configure_repo_dicomsdl_import()

from dicomsdl.vtk_bridge import read_series_volume, to_vtk_image_data


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Read a WG04 single-file DICOM image with DicomSDL and convert it into a VTK image.",
    )
    add_sample_selection_args(parser)
    parser.add_argument(
        "--stored-value",
        action="store_true",
        help="Keep stored pixel values instead of applying modality rescale when present.",
    )
    args = parser.parse_args(argv)

    try:
        try:
            from vtkmodules.util.numpy_support import vtk_to_numpy
        except ImportError:
            from vtk.util.numpy_support import vtk_to_numpy  # type: ignore
        sample_file, discovered = resolve_sample_file(
            args.path,
            args.sample_index,
            args.sample_name,
            download_missing=args.download_missing,
        )
        volume = read_series_volume(sample_file, to_modality_value=not args.stored_value)
        image = to_vtk_image_data(volume)
    except (FileNotFoundError, IndexError, NotADirectoryError, RuntimeError, ValueError, NotImplementedError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    scalars = image.GetPointData().GetScalars()
    if scalars is None:
        print(f"error: VTK did not produce scalar pixel data for {sample_file}", file=sys.stderr)
        return 2

    dims = tuple(int(value) for value in image.GetDimensions())
    components = int(scalars.GetNumberOfComponents())
    flat = vtk_to_numpy(scalars)
    if components == 1:
        array_view = flat.reshape(dims[2], dims[1], dims[0])
    else:
        array_view = flat.reshape(dims[2], dims[1], dims[0], components)

    print("Source reader: DicomSDL")
    print("Target object: vtkImageData")
    print("Bridge module: dicomsdl.vtk_bridge")
    print(f"Sample file: {sample_file}")
    print(f"Discovered sample files: {len(discovered)}")
    print(f"Image dimensions (x, y, z): {dims}")
    print(f"Array shape: {tuple(array_view.shape)}")
    print(f"Scalar type: {scalars.GetDataTypeAsString()}")
    print(f"Scalar components: {components}")
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
