#!/usr/bin/env python3
"""Benchmark single-file WG04 loading paths for reference readers and DicomSDL bridges."""

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
from _wg04_reference_common import add_sample_selection_args, resolve_sample_file

configure_repo_dicomsdl_import()

from dicomsdl.simpleitk_bridge import read_series_volume, to_simpleitk_image
from dicomsdl.vtk_bridge import to_vtk_image_data


@dataclass(slots=True)
class BenchmarkResult:
    mode: str
    runs: list[float]
    shape: tuple[int, ...]
    dtype: str
    values: int


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


def _reshape_vtk_output(image: object, vtk_to_numpy: Callable[[object], np.ndarray]) -> np.ndarray:
    scalars = image.GetPointData().GetScalars()
    if scalars is None:
        raise RuntimeError("VTK did not produce scalar data")

    dims = tuple(int(value) for value in image.GetDimensions())
    components = int(scalars.GetNumberOfComponents())
    flat = vtk_to_numpy(scalars)
    if components == 1:
        return flat.reshape(dims[2], dims[1], dims[0])
    return flat.reshape(dims[2], dims[1], dims[0], components)


def _simpleitk_reference(sample_file: Path) -> np.ndarray:
    sitk = _load_simpleitk()
    image = sitk.ReadImage(str(sample_file))
    return np.asarray(sitk.GetArrayViewFromImage(image))


def _simpleitk_gdcm_reference(sample_file: Path) -> np.ndarray:
    sitk = _load_simpleitk()
    if "GDCMImageIO" not in sitk.ImageFileReader().GetRegisteredImageIOs():
        raise RuntimeError("SimpleITK GDCMImageIO is not registered in this environment")

    reader = sitk.ImageFileReader()
    reader.SetImageIO("GDCMImageIO")
    reader.SetFileName(str(sample_file))
    image = reader.Execute()
    return np.asarray(sitk.GetArrayViewFromImage(image))


def _vtk_reference(sample_file: Path) -> np.ndarray:
    vtk_dicom_image_reader, vtk_to_numpy = _load_vtk()
    reader = vtk_dicom_image_reader()
    reader.SetFileName(str(sample_file))
    reader.Update()
    return _reshape_vtk_output(reader.GetOutput(), vtk_to_numpy)


def _vtk_gdcm_reference(sample_file: Path) -> np.ndarray:
    vtk_gdcm_image_reader = _load_vtk_gdcm_reader()
    _, vtk_to_numpy = _load_vtk()
    reader = vtk_gdcm_image_reader()
    reader.SetFileName(str(sample_file))
    reader.Update()
    return _reshape_vtk_output(reader.GetOutput(), vtk_to_numpy)


def _dicomsdl_image(sample_file: Path) -> np.ndarray:
    return read_series_volume(sample_file, to_modality_value=True).array


def _dicomsdl_to_simpleitk(sample_file: Path) -> np.ndarray:
    sitk = _load_simpleitk()
    volume = read_series_volume(sample_file, to_modality_value=True)
    image = to_simpleitk_image(volume)
    return np.asarray(sitk.GetArrayViewFromImage(image))


def _dicomsdl_to_vtk(sample_file: Path) -> np.ndarray:
    _, vtk_to_numpy = _load_vtk()
    volume = read_series_volume(sample_file, to_modality_value=True)
    image = to_vtk_image_data(volume)
    return _reshape_vtk_output(image, vtk_to_numpy)


def _mode_functions() -> dict[str, Callable[[Path], np.ndarray]]:
    return {
        "simpleitk_reference": _simpleitk_reference,
        "simpleitk_gdcm_reference": _simpleitk_gdcm_reference,
        "vtk_reference": _vtk_reference,
        "vtk_gdcm_reference": _vtk_gdcm_reference,
        "dicomsdl_image": _dicomsdl_image,
        "dicomsdl_to_simpleitk": _dicomsdl_to_simpleitk,
        "dicomsdl_to_vtk": _dicomsdl_to_vtk,
    }


def _benchmark_mode(
    mode: str,
    loader: Callable[[Path], np.ndarray],
    sample_file: Path,
    *,
    warmup: int,
    repeat: int,
) -> BenchmarkResult:
    for _ in range(warmup):
        _ = loader(sample_file)

    runs: list[float] = []
    last_array: np.ndarray | None = None
    for _ in range(repeat):
        start = time.perf_counter()
        last_array = np.asarray(loader(sample_file))
        runs.append(time.perf_counter() - start)

    if last_array is None:
        raise RuntimeError(f"benchmark mode did not produce output: {mode}")

    return BenchmarkResult(
        mode=mode,
        runs=runs,
        shape=tuple(int(value) for value in last_array.shape),
        dtype=str(last_array.dtype),
        values=int(last_array.size),
    )


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Benchmark WG04 single-file loading for SimpleITK/VTK reference readers and DicomSDL bridge paths.",
    )
    add_sample_selection_args(parser)
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
        sample_file, discovered = resolve_sample_file(
            args.path,
            args.sample_index,
            args.sample_name,
            download_missing=args.download_missing,
        )
    except (FileNotFoundError, IndexError, NotADirectoryError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    mode_functions = _mode_functions()
    selected_modes = [mode for mode in args.mode]

    print(f"Sample file: {sample_file}")
    print(f"Discovered sample files: {len(discovered)}")
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
                sample_file,
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
        mvalues_s = (result.values / 1_000_000.0) / mean_s if mean_s > 0 else 0.0
        print(
            f"{result.mode}: shape={result.shape} dtype={result.dtype} "
            f"mean={mean_s:.4f}s median={median_s:.4f}s min={min_s:.4f}s max={max_s:.4f}s "
            f"MValues/s={mvalues_s:.2f}"
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
