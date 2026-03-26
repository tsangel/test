#!/usr/bin/env python3
"""Build a PET volume with DicomSDL, then hand it to SimpleITK."""

from __future__ import annotations

import argparse
import sys

from _repo_dicomsdl_import import configure_repo_dicomsdl_import
from _pet_series_reference_common import add_series_selection_args, count_dicom_files, resolve_series_dir

configure_repo_dicomsdl_import()

from dicomsdl.simpleitk_bridge import read_series_volume, to_simpleitk_image


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Read a PET DICOM series with DicomSDL and convert it into a SimpleITK image.",
    )
    add_series_selection_args(parser)
    parser.add_argument(
        "--stored-value",
        action="store_true",
        help="Keep stored pixel values instead of applying per-slice PET rescale to modality values.",
    )
    args = parser.parse_args(argv)

    try:
        series_dir, discovered = resolve_series_dir(args.path, args.series_index, args.series_name)
        volume = read_series_volume(series_dir, to_modality_value=not args.stored_value)
        image = to_simpleitk_image(volume)
    except (FileNotFoundError, IndexError, NotADirectoryError, RuntimeError, ValueError, NotImplementedError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    print("Source reader: DicomSDL")
    print("Target object: SimpleITK Image")
    print("Bridge module: dicomsdl.simpleitk_bridge")
    print(f"Series directory: {series_dir}")
    print(f"Discovered series dirs: {len(discovered)}")
    print(f"DICOM files: {count_dicom_files(series_dir)}")
    print(f"Image size (x, y, z): {tuple(image.GetSize())}")
    print(f"Array shape (z, y, x): {tuple(volume.array.shape)}")
    print(f"Array dtype: {volume.array.dtype}")
    print(f"Spacing: {tuple(image.GetSpacing())}")
    print(f"Origin: {tuple(image.GetOrigin())}")
    print(f"Direction: {tuple(image.GetDirection())}")
    print(f"Value range: min={volume.array.min()} max={volume.array.max()}")
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
