# DICOM Segmentation (SEG) and Geometry

Use these APIs when you need to inspect DICOM Segmentation (SEG) metadata,
decode SEG frames, build image-plane geometry, plan slice stacks, or check
whether masks and images can be overlaid safely.

In DICOM files, these objects use `Modality (0008,0060) = SEG`. The SOP Class
is the more precise storage identifier: Segmentation Storage for BINARY and
FRACTIONAL SEG, and Label Map Segmentation Storage for LABELMAP SEG.

The examples below use Python and build on the ordinary `dicom.read_file()`
workflow. The geometry layer does not build viewers, allocate final output
volumes, choose a dominant grid, or resample masks.

Install NumPy support for the mask examples:

```bash
pip install "dicomsdl[numpy]"
```

Output values depend on the input file. The SEG excerpts below are adapted from
one binary FDG/FBB brain SEG sample.

## Open a DICOM Segmentation (SEG) File

Open DICOM Segmentation Storage or Label Map Segmentation Storage with
`dicom.seg.read_file()`. It returns a `Segmentation` object instead of a plain
`DicomFile`.

```python
from pathlib import Path

import dicomsdl as dicom

seg_path = Path(r"C:\data\sample-seg.dcm")
seg = dicom.seg.read_file(seg_path)

print(seg.is_valid)
print(seg.segmentation_type)
print(seg.fractional_type)
print(seg.maximum_fractional_value)
print(seg.frame_of_reference_uid)
print(seg.rows, seg.columns, seg.segment_count, seg.frame_count)
```

Example output:

```text
True
SegmentationType.binary
SegmentationFractionalType.none
None
1.3.6.1.4.1.43046.3.380371456.2303.1779756601.801016
256 256 97 2885
```

If you already have bytes, use `read_bytes()`:

```python
data = seg_path.read_bytes()
seg = dicom.seg.read_bytes(data, copy=False)
```

In Python, SEG input starts with `read_file()` or `read_bytes()`. There is no
`dicom.seg.from_dicomfile(df)` entry point because it would need to copy and
reparse the existing `DicomFile`.

DicomSDL supports BINARY and FRACTIONAL SEG through Segmentation Storage, and
LABELMAP SEG through Label Map Segmentation Storage. The adapter opens SEG
metadata without scanning all PixelData up front; LABELMAP stored label values
are validated when frames are decoded or scanned, or when
`validate_label_values()` is called explicitly.

## Inspect Segments

In DICOM SEG, `SegmentSequence` describes the labels carried by the object. Each
item is one semantic class, such as a brain structure, tumor, organ, or derived
mask class.

```python
for segment in seg.segments:
    print("number:", segment.number)
    print("label:", segment.label)
    print("description:", segment.description)
    print("algorithm:", segment.algorithm_type, segment.algorithm_name)
    print("category:", segment.property_category)
    print("type:", segment.property_type)
    print("display:", segment.recommended_display_cielab)
    print()
```

Output excerpt:

```text
number: 1
label: Left-Cerebral-White-Matter
description: Left-Cerebral-White-Matter
algorithm: SegmentAlgorithmType.automatic NCM-Brain
category: Code(value='T-D000A', scheme_designator='SRT', meaning='Anatomical Structure')
type: Code(value='T-A2030', scheme_designator='SRT', meaning='Cerebral White Matter')
display: (63266, 32897, 32893)

number: 2
label: Left-Lateral-Ventricle
description: Left-Lateral-Ventricle
algorithm: SegmentAlgorithmType.automatic NCM-Brain
category: Code(value='T-D000A', scheme_designator='SRT', meaning='Anatomical Structure')
type: Code(value='T-A1600', scheme_designator='SRT', meaning='Brain ventricle')
display: (19516, 47118, 22528)

...
```

You can also look up a segment by its DICOM segment number:

```python
left_white_matter = seg.segment_by_number(1)
if left_white_matter is not None:
    print(left_white_matter.label)
```

Example output:

```text
Left-Cerebral-White-Matter
```

Segment numbers are not the same as Python list indices. Prefer
`segment.number` when you are matching frames to labels.

## Inspect SEG Frames

SEG Pixel Data is multi-frame. For BINARY and FRACTIONAL SEG, each stored frame
belongs to one referenced segment number. A segment usually has many frames,
often one per stored slice.

```python
frame = seg.frames[0]

print(frame.index)
print(frame.referenced_segment_number)
print(frame.image_position_patient)
print(frame.image_orientation_patient)
print(frame.pixel_spacing)
print(frame.slice_thickness)
```

Example output:

```text
0
1
(-128.000061, -131.25, -38.999939)
(1.0, 0.0, 0.0, 0.0, 1.0, 0.0)
(1.0, 1.0)
1.0
```

Use `present_segment_numbers()` when you want code that also works for
LABELMAP SEG. For BINARY and FRACTIONAL SEG it returns the declared
`ReferencedSegmentNumber`; for LABELMAP SEG it returns the non-background label
values actually present in that frame. `referenced_segment_number` is a
compatibility accessor and is not defined for LABELMAP frames.

```python
print(frame.present_segment_numbers())
```

Example output for a BINARY frame:

```text
(1,)
```

Frames can also contain source image references:

```python
for ref in frame.source_images:
    print(ref.sop_class_uid)
    print(ref.sop_instance_uid)
    print(ref.referenced_frame_numbers)
```

Example output:

```text
1.2.840.10008.5.1.4.1.1.2
1.2.840.113619.2.80.981715802.8664.151072595.1914331.90
[]
```

Source image references are provenance metadata. They tell you which images were
used to create the SEG, but they are not the only possible display target. For
overlay, compare `FrameOfReferenceUID` first.

## Decode a BINARY SEG Mask

For BINARY SEG, DICOM stores native 1-bit pixels. DicomSDL returns an unpacked
`uint8` mask with values `0` and `1`.

```python
mask = seg.to_array(0)
print(mask.shape, mask.dtype)
print(mask.min(), mask.max())
```

Example output:

```text
(256, 256) uint8
0 1
```

Decode all frames for one segment:

```python
masks_for_segment = []

for frame in seg.frames_for_segment(1):
    mask = frame.to_array()
    masks_for_segment.append((frame.index, mask))

print("decoded frames:", len(masks_for_segment))
```

Example output:

```text
decoded frames: 87
```

If you want to reuse an output array:

```python
import numpy as np

out = np.empty((seg.rows, seg.columns), dtype=np.uint8)
returned = seg.decode_frame_into(0, out)
print(returned is out)
print(out.shape, out.dtype, out.min(), out.max())
```

Example output:

```text
True
(256, 256) uint8 0 1
```

## Decode a FRACTIONAL SEG Mask

FRACTIONAL SEG stores 8-bit raw samples. DicomSDL returns those raw samples and
leaves scaling to the caller.

```python
import numpy as np

if seg.segmentation_type is dicom.seg.SegmentationType.fractional:
    if not seg.maximum_fractional_value:
        raise ValueError("MaximumFractionalValue is missing")

    raw = seg.to_array(0)  # dtype uint8
    values = raw.astype(np.float32) / float(seg.maximum_fractional_value)
    print(values.min(), values.max())
```

The sample output above is from a BINARY SEG, so this FRACTIONAL block prints
nothing for that file. On a FRACTIONAL SEG whose raw values span the full stored
range, the output would look like:

```text
0.0 1.0
```

This keeps probability and occupancy workflows explicit. The caller can choose
`float32`, `float64`, thresholded boolean masks, or another downstream layout.

## Decode a LABELMAP SEG Frame

LABELMAP SEG stores label values directly in PixelData. Label value `0` is
background; non-zero values correspond to `SegmentSequence` segment numbers.
DicomSDL preserves the stored representation: 8-bit label maps decode to
`uint8`, and 16-bit label maps decode to native-endian `uint16`.

```python
if seg.segmentation_type is dicom.seg.SegmentationType.labelmap:
    labels = seg.to_array(0)
    print(labels.shape, labels.dtype)
    print(seg.present_segment_numbers(0))
```

Example output:

```text
(512, 512) uint16
(1, 24, 300)
```

Palette lookup, color mapping, opacity, and legend rendering are viewer/UI
responsibilities. DicomSDL returns stored label samples and metadata; it does
not render a palette image.

To get a semantic mask for one segment, use `mask_for_segment()`. This API works
across BINARY, FRACTIONAL, and LABELMAP SEG. For FRACTIONAL SEG, the threshold
is applied in normalized `[0, 1]` units.

```python
segment_number = 24
mask = seg.mask_for_segment(0, segment_number)
print(mask.shape, mask.dtype, mask.min(), mask.max())
```

Example output:

```text
(512, 512) uint8 0 1
```

`present_segment_numbers(frame)` scans only the requested LABELMAP frame and
caches the result. `frames_for_segment(segment_number)` and
`validate_label_values()` may scan all LABELMAP frames on first use, so treat
them as explicit validation/indexing operations for large multi-frame SEG
objects.

## Convert DICOM Plane Tags to Geometry

The geometry module turns DICOM image plane metadata into validated objects.

```python
g = dicom.geometry

image = dicom.read_file(r"C:\data\CT-slice-0035.dcm")
plane = g.plane_from_single_frame_image(image)

world = plane.world_from_index(g.ImagePoint2D(100.0, 120.0))
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

## Check SEG/Image Overlay Compatibility

Overlay checks use already-built geometry and frame-of-reference UIDs. They do
not walk datasets and are intended to stay lightweight.

```python
from pathlib import Path

g = dicom.geometry

seg = dicom.seg.read_file(r"C:\data\sample-seg.dcm")
image_files = [dicom.read_file(path) for path in sorted(Path(r"C:\data\CT").glob("*.dcm"))]
image_plan = g.plan_slice_stack(image_files)
if not image_plan.ok or image_plan.volume_geometry is None:
    raise ValueError(f"target image stack is not a uniform volume: {image_plan.status}")

seg_frame = seg.frames[0]
seg_plane = g.plane_from_seg_frame(seg, seg_frame.index)
target_volume = image_plan.volume_geometry

check = g.check_overlay_compatibility(
    seg.frame_of_reference_uid or "",
    seg_plane,
    image_plan.frame_of_reference_uid,
    target_volume,
)

print(check.status)
print("can_transform:", check.can_transform)
print("can_direct_overlay:", check.can_direct_overlay)
print("requires_resampling:", check.requires_resampling)
print("overlaps_extent:", check.overlaps_extent)
if check.target_k_range is None:
    print("target_k_range: None")
else:
    print("target_k_range:", check.target_k_range.begin, check.target_k_range.end)
```

Example output for a same-grid SEG frame:

```text
OverlayCompatibility.compatible
can_transform: True
can_direct_overlay: True
requires_resampling: False
overlaps_extent: True
target_k_range: 35 36
```

Read these fields as:

- `can_direct_overlay`: grids line up closely enough for direct index copy.
- `requires_resampling`: the same frame of reference is usable, but grid mapping
  requires interpolation or resampling.
- `different_extent`: grids line up, but one extent is smaller or larger; this
  is usually crop, pad, or clip policy.
- `different_frame_of_reference`: do not overlay without external registration.

When `can_transform` is true, build the transform once and reuse it inside the
paint or sampling loop:

```python
if check.can_transform:
    transform = g.make_plane_to_volume_transform(seg_plane, target_volume)
    center = g.ImagePoint2D(seg.columns / 2.0, seg.rows / 2.0)
    print(transform.target_index_from_source_index(center))
```

Example output:

```text
ImagePoint3D(i=128, j=128, k=35)
```

## Work with Enhanced Multi-frame Images

Enhanced CT/MR/PET can store many frames in one file. A single file may contain
more than one stack, time point, phase, or echo, so start by grouping frames.

```python
g = dicom.geometry
file = dicom.read_file(r"C:\data\enhanced-ct.dcm")

frame_geometry = g.frame_geometry_from_multiframe_image(file, 0)
print("frame kind:", frame_geometry.kind)

vol_props = g.volumetric_properties_from_multiframe_image(file, 0)
print("volumetric properties:", vol_props.value)

stacks = g.analyze_image_frame_stacks(file)
print("status:", stacks.status)
print("groups:", len(stacks.groups))

for group_index, group in enumerate(stacks.groups):
    print("group:", group_index)
    print("stack_id:", group.key.stack_id)
    print("frames:", group.frame_indices)
    print("analysis:", group.analysis.status)

    if group.analysis.ok:
        plan = g.plan_image_frame_stack(file, group.frame_indices)
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
single_plan = g.plan_image_frame_stack(file)
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

- `missing_geometry`: required plane tags could not be resolved.
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

## Build Synthetic Geometry for Tests

You do not need a DICOM file to test overlay math. You can build geometry
objects directly.

```python
g = dicom.geometry

seg_plane = g.make_image_plane_geometry(
    g.Point3d(0.0, 0.0, 20.0),
    g.Vec3d(1.0, 0.0, 0.0),
    g.Vec3d(0.0, 1.0, 0.0),
    g.ImageSpacing2D(1.0, 1.0),
    g.ImageSize2D(64, 64),
)

image_volume = g.make_image_volume_geometry(
    g.Point3d(0.0, 0.0, 0.0),
    g.Vec3d(1.0, 0.0, 0.0),
    g.Vec3d(0.0, 1.0, 0.0),
    g.Vec3d(0.0, 0.0, 1.0),
    g.ImageSpacing3D(1.0, 1.0, 5.0),
    g.ImageSize3D(64, 64, 16),
)

check = g.check_overlay_compatibility("1.2.3", seg_plane, "1.2.3", image_volume)
if check.target_k_range is not None:
    print(check.status, check.target_k_range.begin, check.target_k_range.end)

transform = g.make_plane_to_volume_transform(seg_plane, image_volume)
print(transform.target_index_from_source_index(g.ImagePoint2D(10.0, 20.0)))
```

Expected output:

```text
OverlayCompatibility.compatible 4 5
ImagePoint3D(i=10, j=20, k=4)
```

This is a simple way to unit-test application code that uses DicomSDL geometry
without carrying real patient data in the test suite.
