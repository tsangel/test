# DICOM Overlay

Use this guide after you have geometry for a source object, such as a SEG frame, and a target image plane or volume. Overlay checks compare already-built geometry and Frame of Reference UIDs; they do not walk datasets or render pixels.

SEG palette/color rendering, opacity, viewport composition, and resampling are application or viewer responsibilities. For SEG mask decoding, see [DICOM Segmentation (SEG)](segmentation.md). For building image and volume geometry, see [DICOM Geometry](geometry.md).

## Check SEG/Image Overlay Compatibility

Overlay checks use already-built geometry and frame-of-reference UIDs. They do
not walk datasets and are intended to stay lightweight.

```python
from pathlib import Path

geom = dicom.geometry

seg = dicom.seg.read_file(r"C:\data\sample-seg.dcm")
image_files = [dicom.read_file(path) for path in sorted(Path(r"C:\data\CT").glob("*.dcm"))]
image_plan = geom.plan_slice_stack(image_files)
if not image_plan.ok or image_plan.volume_geometry is None:
    raise ValueError(f"target image stack is not a uniform volume: {image_plan.status}")

seg_frame = seg.frames[0]
seg_plane = geom.plane_from_seg_frame(seg, seg_frame.index)
target_volume = image_plan.volume_geometry

check = geom.check_overlay_compatibility(
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
    transform = geom.make_plane_to_volume_transform(seg_plane, target_volume)
    center = geom.ImagePoint2D(seg.columns / 2.0, seg.rows / 2.0)
    print(transform.target_index_from_source_index(center))
```

Example output:

```text
ImagePoint3D(i=128, j=128, k=35)
```

## Build Synthetic Geometry for Tests

You do not need a DICOM file to test overlay math. You can build geometry
objects directly.

```python
geom = dicom.geometry

seg_plane = geom.make_image_plane_geometry(
    geom.Point3d(0.0, 0.0, 20.0),
    geom.Vec3d(1.0, 0.0, 0.0),
    geom.Vec3d(0.0, 1.0, 0.0),
    geom.ImageSpacing2D(1.0, 1.0),
    geom.ImageSize2D(64, 64),
)

image_volume = geom.make_image_volume_geometry(
    geom.Point3d(0.0, 0.0, 0.0),
    geom.Vec3d(1.0, 0.0, 0.0),
    geom.Vec3d(0.0, 1.0, 0.0),
    geom.Vec3d(0.0, 0.0, 1.0),
    geom.ImageSpacing3D(1.0, 1.0, 5.0),
    geom.ImageSize3D(64, 64, 16),
)

check = geom.check_overlay_compatibility("1.2.3", seg_plane, "1.2.3", image_volume)
if check.target_k_range is not None:
    print(check.status, check.target_k_range.begin, check.target_k_range.end)

transform = geom.make_plane_to_volume_transform(seg_plane, image_volume)
print(transform.target_index_from_source_index(geom.ImagePoint2D(10.0, 20.0)))
```

Expected output:

```text
OverlayCompatibility.compatible 4 5
ImagePoint3D(i=10, j=20, k=4)
```

This is a simple way to unit-test application code that uses DicomSDL geometry
without carrying real patient data in the test suite.
