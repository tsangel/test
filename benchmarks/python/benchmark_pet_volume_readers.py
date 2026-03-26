#!/usr/bin/env python3
"""Benchmark PET volume loading paths for reference readers and DicomSDL bridges."""

from __future__ import annotations

import argparse
import statistics
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

import numpy as np

_THIS_DIR = Path(__file__).resolve().parent
_REPO_ROOT = _THIS_DIR.parents[1]
_EXAMPLES_PY = _REPO_ROOT / "examples" / "python" / "itk_vtk"
if str(_EXAMPLES_PY) not in sys.path:
    sys.path.insert(0, str(_EXAMPLES_PY))

from _repo_dicomsdl_import import configure_repo_dicomsdl_import
from _pet_series_reference_common import add_series_selection_args, count_dicom_files, resolve_series_dir

configure_repo_dicomsdl_import()

from dicomsdl.simpleitk_bridge import read_series_volume, to_simpleitk_image
from dicomsdl.vtk_bridge import to_vtk_image_data


@dataclass(slots=True)
class BenchmarkResult:
    mode: str
    runs: list[float]
    shape: tuple[int, ...]
    dtype: str
    voxels: int


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


def _load_vtk_string_array():
    try:
        from vtkmodules.vtkCommonCore import vtkStringArray
    except ImportError:
        try:
            import vtk  # type: ignore
        except ImportError as exc:
            raise RuntimeError(
                "VTK is not installed. Install it with: pip install vtk"
            ) from exc
        vtk_string_array = vtk.vtkStringArray
    else:
        vtk_string_array = vtkStringArray
    return vtk_string_array


def _load_vtk_gdcm_reader():
    try:
        from vtkmodules.vtkIOGDCM import vtkGDCMImageReader
    except ImportError:
        try:
            import vtk  # type: ignore
        except ImportError as exc:
            raise RuntimeError(
                "VTK is not installed. Install it with: pip install vtk"
            ) from exc
        vtk_gdcm_image_reader = getattr(vtk, "vtkGDCMImageReader", None)
        if vtk_gdcm_image_reader is None:
            try:
                from vtkgdcm import vtkGDCMImageReader  # type: ignore
            except ImportError as exc:
                raise RuntimeError(
                    "VTK GDCM reader is not available in this environment. "
                    "Install or build a VTK package with vtkGDCMImageReader/vtkIOGDCM support, "
                    "or install the separate vtkgdcm Python module."
                ) from exc
            vtk_gdcm_image_reader = vtkGDCMImageReader
    else:
        vtk_gdcm_image_reader = vtkGDCMImageReader
    return vtk_gdcm_image_reader


def _series_file_names(series_dir: Path) -> list[str]:
    file_names = [
        str(child)
        for child in sorted(series_dir.iterdir())
        if child.is_file() and child.suffix.lower() == ".dcm"
    ]
    if not file_names:
        raise RuntimeError(f"No DICOM files found under {series_dir}")
    return file_names


def _simpleitk_reference(series_dir: Path) -> np.ndarray:
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
    return np.asarray(sitk.GetArrayViewFromImage(image))


def _simpleitk_gdcm_reference(series_dir: Path) -> np.ndarray:
    sitk = _load_simpleitk()
    if "GDCMImageIO" not in sitk.ImageFileReader().GetRegisteredImageIOs():
        raise RuntimeError("SimpleITK GDCMImageIO is not registered in this environment")

    series_ids = list(sitk.ImageSeriesReader.GetGDCMSeriesIDs(str(series_dir)) or [])
    if series_ids:
        file_names = list(sitk.ImageSeriesReader.GetGDCMSeriesFileNames(str(series_dir), series_ids[0]))
    else:
        file_names = list(sitk.ImageSeriesReader.GetGDCMSeriesFileNames(str(series_dir)))
    if not file_names:
        raise RuntimeError(f"SimpleITK could not enumerate DICOM files under {series_dir}")

    reader = sitk.ImageSeriesReader()
    reader.SetImageIO("GDCMImageIO")
    reader.SetFileNames(file_names)
    reader.MetaDataDictionaryArrayUpdateOn()
    reader.LoadPrivateTagsOn()
    image = reader.Execute()
    return np.asarray(sitk.GetArrayViewFromImage(image))


def _vtk_reference(series_dir: Path) -> np.ndarray:
    vtk_dicom_image_reader, _, _, vtk_to_numpy = _load_vtk()
    reader = vtk_dicom_image_reader()
    reader.SetDirectoryName(str(series_dir))
    reader.Update()
    image = reader.GetOutput()
    dims = tuple(int(value) for value in image.GetDimensions())
    scalars = image.GetPointData().GetScalars()
    if scalars is None:
        raise RuntimeError(f"VTK did not produce scalar data for {series_dir}")
    return vtk_to_numpy(scalars).reshape(dims[2], dims[1], dims[0])


def _vtk_gdcm_reference(series_dir: Path) -> np.ndarray:
    vtk_gdcm_image_reader = _load_vtk_gdcm_reader()
    _, _, _, vtk_to_numpy = _load_vtk()
    reader = vtk_gdcm_image_reader()
    if hasattr(reader, "SetDirectoryName"):
        reader.SetDirectoryName(str(series_dir))
    elif hasattr(reader, "SetFileNames"):
        vtk_string_array = _load_vtk_string_array()
        file_names = vtk_string_array()
        for file_name in _series_file_names(series_dir):
            file_names.InsertNextValue(file_name)
        reader.SetFileNames(file_names)
    else:
        raise RuntimeError("VTK GDCM reader does not support directory or filename-array input")
    reader.Update()
    image = reader.GetOutput()
    dims = tuple(int(value) for value in image.GetDimensions())
    scalars = image.GetPointData().GetScalars()
    if scalars is None:
        raise RuntimeError(f"VTK GDCM reader did not produce scalar data for {series_dir}")
    return vtk_to_numpy(scalars).reshape(dims[2], dims[1], dims[0])


def _dicomsdl_volume(series_dir: Path) -> np.ndarray:
    return read_series_volume(series_dir, to_modality_value=True).array


def _dicomsdl_to_simpleitk(series_dir: Path) -> np.ndarray:
    sitk = _load_simpleitk()
    volume = read_series_volume(series_dir, to_modality_value=True)
    image = to_simpleitk_image(volume)
    return np.asarray(sitk.GetArrayViewFromImage(image))


def _dicomsdl_to_vtk(series_dir: Path) -> np.ndarray:
    _, _, _, vtk_to_numpy = _load_vtk()
    volume = read_series_volume(series_dir, to_modality_value=True)
    image = to_vtk_image_data(volume)
    scalars = image.GetPointData().GetScalars()
    if scalars is None:
        raise RuntimeError("DicomSDL bridge did not produce VTK scalar data")
    return vtk_to_numpy(scalars).reshape(volume.array.shape)


def _mode_functions() -> dict[str, Callable[[Path], np.ndarray]]:
    return {
        "simpleitk_reference": _simpleitk_reference,
        "simpleitk_gdcm_reference": _simpleitk_gdcm_reference,
        "vtk_reference": _vtk_reference,
        "vtk_gdcm_reference": _vtk_gdcm_reference,
        "dicomsdl_volume": _dicomsdl_volume,
        "dicomsdl_to_simpleitk": _dicomsdl_to_simpleitk,
        "dicomsdl_to_vtk": _dicomsdl_to_vtk,
    }


def _benchmark_mode(
    mode: str,
    loader: Callable[[Path], np.ndarray],
    series_dir: Path,
    *,
    warmup: int,
    repeat: int,
) -> BenchmarkResult:
    for _ in range(warmup):
        _ = loader(series_dir)

    runs: list[float] = []
    last_array: np.ndarray | None = None
    for _ in range(repeat):
        start = time.perf_counter()
        last_array = np.asarray(loader(series_dir))
        runs.append(time.perf_counter() - start)

    if last_array is None:
        raise RuntimeError(f"benchmark mode did not produce output: {mode}")

    return BenchmarkResult(
        mode=mode,
        runs=runs,
        shape=tuple(int(value) for value in last_array.shape),
        dtype=str(last_array.dtype),
        voxels=int(last_array.size),
    )


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Benchmark PET volume loading for SimpleITK/VTK reference readers and DicomSDL bridge paths.",
    )
    add_series_selection_args(parser)
    parser.add_argument(
        "--mode",
        nargs="+",
        choices=tuple(_mode_functions().keys()),
        default=list(_mode_functions().keys()),
        help="One or more benchmark modes to run",
    )
    parser.add_argument("--warmup", type=int, default=1, help="Warmup runs per mode")
    parser.add_argument("--repeat", type=int, default=5, help="Measured runs per mode")
    args = parser.parse_args(argv)

    try:
        series_dir, discovered = resolve_series_dir(args.path, args.series_index, args.series_name)
    except (FileNotFoundError, IndexError, NotADirectoryError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    mode_functions = _mode_functions()
    selected_modes = [mode for mode in args.mode]

    print(f"Series directory: {series_dir}")
    print(f"Discovered series dirs: {len(discovered)}")
    print(f"DICOM files: {count_dicom_files(series_dir)}")
    print(f"Warmup: {args.warmup}")
    print(f"Repeat: {args.repeat}")
    print()

    results: list[BenchmarkResult] = []
    failures: list[tuple[str, str]] = []
    for mode in selected_modes:
        try:
            result = _benchmark_mode(
                mode,
                mode_functions[mode],
                series_dir,
                warmup=args.warmup,
                repeat=args.repeat,
            )
        except RuntimeError as exc:
            failures.append((mode, str(exc)))
            continue

        results.append(result)
        mean_s = statistics.mean(result.runs)
        median_s = statistics.median(result.runs)
        min_s = min(result.runs)
        max_s = max(result.runs)
        ms_per_slice = (mean_s / result.shape[0]) * 1000.0 if result.shape else 0.0
        mpix_s = (result.voxels / 1_000_000.0) / mean_s if mean_s > 0 else 0.0
        print(
            f"{result.mode}: shape={result.shape} dtype={result.dtype} "
            f"mean={mean_s:.4f}s median={median_s:.4f}s min={min_s:.4f}s max={max_s:.4f}s "
            f"ms/slice={ms_per_slice:.3f} MPix/s={mpix_s:.2f}"
        )

    if results:
        print()
        print("Fastest to slowest by mean runtime:")
        for result in sorted(results, key=lambda item: statistics.mean(item.runs)):
            print(f"  {result.mode}: {statistics.mean(result.runs):.4f}s")

    if failures:
        print()
        print("Skipped modes:")
        for mode, reason in failures:
            print(f"  {mode}: {reason}")

    return 0 if results else 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
