# DICOM Geometry

DicomSDL builds geometry objects from DICOM Image Plane attributes such as
`ImagePositionPatient`, `ImageOrientationPatient`, `PixelSpacing`, `Rows`, and
`Columns`.

`ImagePlaneGeometry` carries the plane origin, row/column directions, normal,
pixel spacing, image size, and index/world transforms. `ImageVolumeGeometry`
adds a slice direction, slice spacing, slice count, and 3D index/world
transforms for rectilinear volumes.

## Build Geometry from Image Plane Attributes

The geometry module turns DICOM Image Plane attributes into validated objects.

```python
geom = dicom.geometry

image = dicom.read_file(r"C:\data\CT-slice-0035.dcm")
plane = geom.plane_from_single_frame_image(image)

world = plane.world_from_index(geom.ImagePoint2D(100.0, 120.0))
index = plane.index_from_world(world)

print(world)
print(index)
```

Example output for a 1 mm axial slice:

```text
Point3d(x=-28.000061, y=-11.25, z=-38.999939)
ImagePoint2D(i=100, j=120)
```

DicomSDL uses these index names:

- `i`: column-like image index.
- `j`: row-like image index.
- `k`: slice/frame index for volumes.

DICOM `PixelSpacing` is `[row_spacing, column_spacing]`, so DicomSDL maps it
to `spacing_j` and `spacing_i`.

```python
print("columns, rows:", plane.columns, plane.rows)
print("spacing_i, spacing_j:", plane.spacing_i, plane.spacing_j)
```

Example output:

```text
columns, rows: 512 512
spacing_i, spacing_j: 1.0 1.0
```

## Plan a Classic Slice Stack

Classic CT/MR/PET studies are often one SOP instance per slice. `plan_slice_stack`
sorts those files into physical slice order and returns a volume geometry plus a
placement list.

```python
from pathlib import Path

series_dir = Path(r"C:\data\CT")
paths = sorted(series_dir.glob("*.dcm"))
files = [dicom.read_file(path) for path in paths]

plan = dicom.geometry.plan_slice_stack(files)

if not plan.ok:
    print("stack failed:", plan.status)
    for issue in plan.issues:
        print(issue.status, issue.source_index, issue.frame_index, issue.tag, issue.message)
    raise SystemExit

volume_geometry = plan.volume_geometry
assert volume_geometry is not None

print(volume_geometry.columns, volume_geometry.rows, volume_geometry.slices)
print(volume_geometry.spacing_i, volume_geometry.spacing_j, volume_geometry.spacing_k)

for item in plan.placements[:5]:
    print("source file", item.source_index, "frame", item.frame_index, "-> k", item.target_k)
```

Example output:

```text
256 256 91
1.0 1.0 1.0
source file 0 frame 0 -> k 0
source file 1 frame 0 -> k 1
source file 2 frame 0 -> k 2
source file 3 frame 0 -> k 3
source file 4 frame 0 -> k 4
```

To assemble pixels yourself:

```python
import numpy as np

volume = np.empty(
    (volume_geometry.slices, volume_geometry.rows, volume_geometry.columns),
    dtype=files[0].to_array(frame=0).dtype,
)

for item in plan.placements:
    volume[item.target_k] = files[item.source_index].to_array(frame=item.frame_index)

print(volume.shape, volume.dtype)
```

Example output:

```text
(91, 256, 256) uint16
```

The geometry layer does not decode pixels or allocate the output volume. That
keeps viewers, batch jobs, and resampling pipelines free to choose their own
memory layout and policy.

## Work with Enhanced Multi-frame Images

Enhanced CT/MR/PET can store many frames in one file. A single file may contain
more than one stack, time point, phase, or echo, so start by grouping frames.

```python
geom = dicom.geometry
file = dicom.read_file(r"C:\data\enhanced-ct.dcm")

frame_geometry = geom.frame_geometry_from_multiframe_image(file, 0)
print("frame kind:", frame_geometry.kind)

vol_props = geom.volumetric_properties_from_multiframe_image(file, 0)
print("volumetric properties:", vol_props.value)

stacks = geom.analyze_image_frame_stacks(file)
print("status:", stacks.status)
print("groups:", len(stacks.groups))

for group_index, group in enumerate(stacks.groups):
    print("group:", group_index)
    print("stack_id:", group.key.stack_id)
    print("frames:", group.frame_indices)
    print("analysis:", group.analysis.status)

    if group.analysis.ok:
        plan = geom.plan_image_frame_stack(file, group.frame_indices)
        print("plan:", plan.status, "placements:", len(plan.placements))
```

Example output for a single enhanced CT stack:

```text
frame kind: ImageFrameGeometryKind.regular_plane
volumetric properties: VolumetricPropertiesValue.volume
status: SliceStackStatus.ok
groups: 1
group: 0
stack_id: STACK_A
frames: [0, 1, 2, 3, 4, ...]
analysis: SliceStackStatus.ok
plan: SliceStackStatus.ok placements: 120
```

`plane_from_multiframe_image(file, frame_index)` is the direct overlay helper.
It returns only regular slice planes. `frame_geometry_from_multiframe_image()`
keeps the `ImageFrameGeometryKind` so callers can inspect sampled projection or
distorted frame metadata without accidentally treating it as a normal slice.

Use `plan_image_frame_stack(file)` only when the whole file is expected to be
one stack. If the file may contain multiple groups, plan each group explicitly.

```python
single_plan = geom.plan_image_frame_stack(file)
if not single_plan.ok:
    for issue in single_plan.issues:
        print(issue.status, issue.frame_index, issue.tag, issue.message)
```

If the file contains more than one stack, output may look like:

```text
SliceStackStatus.multiple_frame_stacks 0 (0020,9157) file contains multiple frame stacks
```

## Work with Reconstructed NM TOMO Stacks

Nuclear Medicine Image Storage uses older frame organization tags. DicomSDL has
a purpose-built adapter for reconstructed TOMO stacks.

```python
nm = dicom.read_file(r"C:\data\nm-recon-tomo.dcm")
plan = dicom.geometry.plan_nm_frame_stack(nm)

if not plan.ok:
    print("NM stack failed:", plan.status)
    for issue in plan.issues:
        print(issue.status, issue.frame_index, issue.tag, issue.message)
else:
    volume = plan.volume_geometry
    assert volume is not None
    print(volume.columns, volume.rows, volume.slices)
    print([(item.frame_index, item.target_k) for item in plan.placements[:10]])
```

Example output:

```text
128 128 64
[(0, 0), (1, 1), (2, 2), (3, 3), (4, 4), (5, 5), (6, 6), (7, 7), (8, 8), (9, 9)]
```

The current NM adapter accepts `RECON TOMO` and `RECON GATED TOMO` only when
`NumberOfFrames` is present and `FrameIncrementPointer` contains exactly one
value, `SliceVector`. Projection acquisitions and extra vectors such as time or
energy are rejected instead of being silently inferred.

## Diagnose Stack Failures

Analysis objects keep structured issues so a viewer or batch script can decide
what to do next.

```python
analysis = dicom.geometry.analyze_slice_stack(files)

print("status:", analysis.status)
print("max residual:", analysis.max_in_plane_residual_mm)

for run in analysis.uniform_runs:
    print("uniform run:", run.begin_sorted_index, run.end_sorted_index, run.spacing_mm)

for issue in analysis.issues:
    print("status:", issue.status)
    print("source:", issue.source_index, "frame:", issue.frame_index)
    print("tag:", issue.tag)
    print("path depth:", issue.source_depth, "leaf:", issue.source_leaf_tag)
    print("message:", issue.message)
```

Example output for a non-uniform stack:

```text
status: SliceStackStatus.non_uniform_spacing
max residual: 0.0
uniform run: 0 32 1.0
uniform run: 32 64 2.0
status: SliceStackStatus.non_uniform_spacing
source: 32 frame: 0
tag: (0020,0032)
path depth: 0 leaf: (0020,0032)
message: slice spacing is not uniform
```

Typical statuses:

- `missing_geometry`: required Image Plane attributes could not be resolved.
- `missing_dimension_module`: enhanced grouping metadata is missing.
- `multiple_frame_stacks`: a whole-file convenience call saw more than one stack.
- `duplicate_slice_position`: two frames occupy the same slice position.
- `non_uniform_spacing`: the stack is useful to inspect, but not a single
  uniform volume grid.
- `inconsistent_slice_origin`: slice origins drift in-plane and cannot be
  represented by one rectilinear affine volume.

For non-uniform input, `uniform_runs` can still identify useful contiguous
sub-ranges. DicomSDL reports them, but it does not choose a dominant grid or
resample into it.
