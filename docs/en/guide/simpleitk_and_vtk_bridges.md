# SimpleITK and VTK Bridges

DicomSDL can decode DICOM pixel data and hand the result to image-toolkit objects without forcing you to rewrite the rest of your Python workflow.

The two bridge modules are:

- `dicomsdl.simpleitk_bridge`
- `dicomsdl.vtk_bridge`

Use them when you want DicomSDL to own DICOM parsing, slice sorting, pixel decode, and rescale handling, while still continuing downstream work in SimpleITK or VTK.

## What These Bridges Do

The bridge modules share the same internal volume-building path and differ only at the final conversion step.

They can:

- read a single-file 2D image or a file-based DICOM series
- read a stack-like single-file multiframe grayscale volume
- sort slices and assemble a volume
- map spacing, origin, and direction from DICOM geometry
- optionally apply per-slice modality rescale with `to_modality_value=True`
- convert the decoded volume into `SimpleITK.Image` or `vtkImageData`

They are not full drop-in replacements for `sitk.ReadImage(...)` or `vtkDICOMImageReader`.
They are convenience entry points built on top of DicomSDL.

## Install

Install only the toolkit modules you need.

```bash
pip install "dicomsdl[numpy]" SimpleITK vtk
```

If you only use one toolkit, you can install just that one:

```bash
pip install "dicomsdl[numpy]" SimpleITK
pip install "dicomsdl[numpy]" vtk
```

## Shared Behavior

### Input

Pass either:

- a directory that contains a file-based DICOM series
- a single DICOM file for a 2D image
- a single DICOM file that stores a stack-like multiframe grayscale volume

These examples focus on single-file 2D images, stack-like multiframe grayscale
volumes, and file-per-slice series.
If your workflow depends on non-stack multiframe localizers or more complex
enhanced multi-frame layouts, validate that path separately.

### Geometry

The bridges preserve:

- `spacing`
- `origin`
- `direction`

These values are derived from DICOM orientation and position metadata when available.

For stack-like 3D inputs, the bridges canonicalize the volume into physical
stack order:

- slices and frames are ordered by projection onto the base slice normal
- the first output slice has the smallest projected position
- `spacing[2]` stays positive
- the stack axis is carried by `direction`

For file-based series with mixed slice orientations, the dominant orientation
group is used and localizer-like outliers are ignored.

### Output Dtype Policy

The bridges try to preserve practical scalar types instead of promoting everything to float.

- no rescale: keep the stored dtype
- `slope ~= 1` with an integral intercept: keep an integer dtype
- fractional rescale: promote to `float32`

This means the output dtype can differ from toolkit-native readers, which may choose a different scalar type policy.

### Modality Values

Both public readers default to `to_modality_value=True`.

That is especially important for PET, where per-slice rescale often matters.
If you want stored values instead, pass `to_modality_value=False`.

### 2D Images

Single-file 2D images are exposed as single-slice images.
Vector pixel data such as RGB keep their component axis.

### Multiframe Volumes

Stack-like single-file multiframe grayscale volumes are exposed as `(z, y, x)`.
They use the same physical-order canonicalization as file-per-slice series.
If frame positions are not consistent with a single stack direction, the bridge
currently raises `NotImplementedError` instead of guessing a geometry mapping.

## SimpleITK

Public entry points:

- `read_series_image(path, *, to_modality_value=True) -> SimpleITK.Image`
- `to_simpleitk_image(volume) -> SimpleITK.Image`

Example:

```python
from dicomsdl.simpleitk_bridge import read_series_image

image = read_series_image(
    r"..\sample\PT\00013 Torso PET AC OSEM",
    to_modality_value=True,
)

print(image.GetSize())
print(image.GetSpacing())
```

If you want access to the intermediate decoded volume first:

```python
from dicomsdl.simpleitk_bridge import read_series_volume, to_simpleitk_image

volume = read_series_volume(r"..\sample\PT\00013 Torso PET AC OSEM")
image = to_simpleitk_image(volume)
```

The result is a normal `SimpleITK.Image`, so you can pass it directly to SimpleITK filters and pipelines.

## VTK

Public entry points:

- `read_series_image_data(path, *, to_modality_value=True, copy=False) -> vtkImageData`
- `to_vtk_image_data(volume, *, copy=False) -> vtkImageData`

Example:

```python
from dicomsdl.vtk_bridge import read_series_image_data

image = read_series_image_data(
    r"..\sample\PT\00013 Torso PET AC OSEM",
    to_modality_value=True,
    copy=False,
)

print(image.GetDimensions())
print(image.GetSpacing())
```

If you want access to the intermediate decoded volume first:

```python
from dicomsdl.vtk_bridge import read_series_volume, to_vtk_image_data

volume = read_series_volume(r"..\sample\PT\00013 Torso PET AC OSEM")
image = to_vtk_image_data(volume, copy=False)
```

`copy=False` is the fast path and uses zero-copy when possible.
Use `copy=True` if you need a VTK-owned copy that is independent from the original NumPy-backed bridge objects.

## Native Readers vs. Bridges

Use the toolkit-native readers when you need full reader-class API compatibility.

Use the DicomSDL bridges when you want:

- DicomSDL to control decode and rescale behavior
- the same geometry and dtype policy across both toolkits
- canonical physical-order 3D volumes with positive slice spacing
- a simple way to compare DicomSDL against native readers in examples or benchmarks

Practical notes:

- the usual SimpleITK DICOM series path already uses GDCM
- VTK comparisons may be against `vtkDICOMImageReader` or `vtkgdcm`, depending on your environment
- geometry can intentionally differ from native reader storage order because the
  bridge canonicalizes stack order before exposing the volume

## Examples and Notebooks

This repository includes ready-to-run examples and notebooks:

- `examples/python/itk_vtk/dicomsdl_to_simpleitk_pet_volume.py`
- `examples/python/itk_vtk/dicomsdl_to_vtk_pet_volume.py`
- `tutorials/basic_vtk3d.ipynb`
- `tutorials/timeit_itk.ipynb`
- `tutorials/timeit_vtk.ipynb`
- `benchmarks/python/benchmark_pet_volume_readers.py`
- `benchmarks/python/benchmark_wg04_readers.py`
