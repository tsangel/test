#!/usr/bin/env python3
"""Reference PET volume reader using SimpleITK."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Iterable

from _pet_series_reference_common import (
    add_series_selection_args,
    count_dicom_files,
    resolve_series_dir,
)


_METADATA_KEYS: tuple[tuple[str, str], ...] = (
    ("Modality", "0008|0060"),
    ("SeriesDescription", "0008|103e"),
    ("CorrectedImage", "0028|0051"),
    ("Units", "0054|1001"),
    ("RescaleIntercept", "0028|1052"),
    ("RescaleSlope", "0028|1053"),
)


def _load_simpleitk():
    try:
        import SimpleITK as sitk
    except ImportError as exc:
        raise RuntimeError(
            "SimpleITK is not installed. Install it with: pip install SimpleITK"
        ) from exc
    return sitk


def _metadata_value(reader: object, slice_index: int, key: str) -> str | None:
    if reader.HasMetaDataKey(slice_index, key):
        return reader.GetMetaData(slice_index, key)
    return None


def _print_metadata(reader: object, slice_index: int, keys: Iterable[tuple[str, str]]) -> None:
    for label, tag in keys:
        value = _metadata_value(reader, slice_index, tag)
        if value is not None:
            print(f"{label}: {value}")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Read a PET DICOM volume with SimpleITK and print a compact summary.",
    )
    add_series_selection_args(parser)
    args = parser.parse_args(argv)

    try:
        sitk = _load_simpleitk()
        series_dir, discovered = resolve_series_dir(args.path, args.series_index, args.series_name)
    except (FileNotFoundError, IndexError, NotADirectoryError, RuntimeError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    series_ids = list(sitk.ImageSeriesReader.GetGDCMSeriesIDs(str(series_dir)) or [])
    if series_ids:
        series_id = series_ids[0]
        file_names = list(sitk.ImageSeriesReader.GetGDCMSeriesFileNames(str(series_dir), series_id))
    else:
        series_id = None
        file_names = list(sitk.ImageSeriesReader.GetGDCMSeriesFileNames(str(series_dir)))

    if not file_names:
        print(f"error: no DICOM files readable by SimpleITK under {series_dir}", file=sys.stderr)
        return 2

    reader = sitk.ImageSeriesReader()
    reader.SetFileNames(file_names)
    reader.MetaDataDictionaryArrayUpdateOn()
    reader.LoadPrivateTagsOn()
    image = reader.Execute()
    array_view = sitk.GetArrayViewFromImage(image)

    print(f"Framework: SimpleITK")
    print(f"Series directory: {series_dir}")
    print(f"Discovered series dirs: {len(discovered)}")
    print(f"DICOM files: {count_dicom_files(series_dir)}")
    if series_id is not None:
        print(f"Series ID: {series_id}")
    print(f"Image size (x, y, z): {tuple(image.GetSize())}")
    print(f"Array shape (z, y, x): {tuple(array_view.shape)}")
    print(f"Pixel type: {image.GetPixelIDTypeAsString()}")
    print(f"Spacing: {tuple(image.GetSpacing())}")
    print(f"Origin: {tuple(image.GetOrigin())}")
    print(f"Direction: {tuple(image.GetDirection())}")
    print(f"Value range: min={array_view.min()} max={array_view.max()}")
    _print_metadata(reader, 0, _METADATA_KEYS)

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

