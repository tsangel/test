#!/usr/bin/env python3
"""Reference WG04 single-file reader using vtkgdcm's vtkGDCMImageReader."""

from __future__ import annotations

import argparse
import sys

from _wg04_reference_common import add_sample_selection_args, resolve_sample_file


def _load_vtk_and_vtkgdcm():
    try:
        from vtkmodules.util.numpy_support import vtk_to_numpy
    except ImportError:
        try:
            from vtk.util.numpy_support import vtk_to_numpy  # type: ignore
        except ImportError as exc:
            raise RuntimeError(
                "VTK is not installed. Install it with: pip install vtk"
            ) from exc

    try:
        import vtkgdcm  # type: ignore
    except ImportError as exc:
        raise RuntimeError(
            "vtkgdcm is not installed in this Python environment. "
            "Use the dicom-bench conda env or install a vtkgdcm build first."
        ) from exc

    return vtkgdcm, vtk_to_numpy


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Read a WG04 single-file DICOM image with vtkgdcm and print a compact summary.",
    )
    add_sample_selection_args(parser)
    args = parser.parse_args(argv)

    try:
        vtkgdcm, vtk_to_numpy = _load_vtk_and_vtkgdcm()
        sample_file, discovered = resolve_sample_file(
            args.path,
            args.sample_index,
            args.sample_name,
            download_missing=args.download_missing,
        )
    except (FileNotFoundError, IndexError, NotADirectoryError, RuntimeError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    reader = vtkgdcm.vtkGDCMImageReader()
    reader.SetFileName(str(sample_file))
    reader.Update()

    image = reader.GetOutput()
    scalars = image.GetPointData().GetScalars()
    if scalars is None:
        print(f"error: vtkgdcm did not produce scalar pixel data for {sample_file}", file=sys.stderr)
        return 2

    dims = tuple(int(value) for value in image.GetDimensions())
    components = int(scalars.GetNumberOfComponents())
    flat = vtk_to_numpy(scalars)
    if components == 1:
        array_view = flat.reshape(dims[2], dims[1], dims[0])
    else:
        array_view = flat.reshape(dims[2], dims[1], dims[0], components)

    print("Framework: vtkgdcm")
    print(f"Sample file: {sample_file}")
    print(f"Discovered sample files: {len(discovered)}")
    print(f"Image dimensions (x, y, z): {dims}")
    print(f"Array shape: {tuple(array_view.shape)}")
    print(f"Scalar type: {scalars.GetDataTypeAsString()}")
    print(f"Scalar components: {components}")
    print(f"Spacing: {tuple(image.GetSpacing())}")
    print(f"Origin: {tuple(image.GetOrigin())}")
    print(f"Value range: min={array_view.min()} max={array_view.max()}")
    if hasattr(reader, "GetShift"):
        print(f"Shift: {reader.GetShift()}")
    if hasattr(reader, "GetScale"):
        print(f"Scale: {reader.GetScale()}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
