# DICOM Geometry Helpers

This page records the first public contract for `dicom_geometry.h` and the
Python `dicom.geometry` module. The geometry layer is intentionally independent
from viewers, viewport state, pixel buffers, and resampling policy.

## Scope

- Build validated 2D image plane and rectilinear 3D volume geometry.
- Resolve classic single-frame image geometry from root DICOM tags.
- Resolve enhanced multi-frame image geometry from
  `PerFrameFunctionalGroupsSequence -> SharedFunctionalGroupsSequence -> root`.
- Resolve DICOM SEG frame geometry without falling back to root image-plane tags.
- Check overlay compatibility using already-built geometry objects.
- Plan uniform slice stacks into a volume geometry and placement list.

The geometry layer does not decode pixels, allocate output volumes, choose a
dominant grid for non-uniform data, or resample masks/images.

## Coordinate Contract

DicomSDL uses image index names `i`, `j`, and `k`:

- `i`: column-like image index.
- `j`: row-like image index.
- `k`: slice/frame index for rectilinear volumes.

For DICOM `ImageOrientationPatient`, DicomSDL maps the first triplet to
`direction_i` and the second triplet to `direction_j`. For DICOM
`PixelSpacing = [row_spacing, column_spacing]`, DicomSDL maps
`spacing_j = row_spacing` and `spacing_i = column_spacing`.

`ImagePlaneGeometry::index_to_world_matrix()` embeds `(i, j, normal_mm, 1)` in
patient/world space. Its inverse returns `(i, j, signed_normal_distance_mm, 1)`.

## C++ Usage

```cpp
#include <dicom.h>
#include <dicom_geometry.h>

auto file = dicom::read_file(path);
auto plane = dicom::geometry::plane_from_single_frame_image(*file);
if (!plane.ok()) {
    // Inspect plane.status(), plane.tag(), plane.message(), plane.source().
}

auto world = plane.value().world_from_index({12.0, 8.0});
auto index = plane.value().index_from_world(world);
```

Enhanced multi-frame image geometry can be read one frame at a time:

```cpp
dicom::geometry::FrameGeometryReader reader(*file);
auto frame0 = reader.plane(0);
```

Use the convenience functions for occasional access, and reuse
`FrameGeometryReader` when reading many frames. `plane()` returns only regular
slice planes and rejects `SAMPLED`/`DISTORTED` frame geometry; use
`image_frame_geometry()` when the caller needs to inspect those frame kinds.

## Python Usage

Python exposes the same concepts through `dicom.geometry`. Construction
failures raise `ValueError` instead of exposing `GeometryBuildResult<T>`.

```python
import dicomsdl as dicom

g = dicom.geometry
plane = g.make_image_plane_geometry(
    g.Point3d(0.0, 0.0, 10.0),
    g.Vec3d(1.0, 0.0, 0.0),
    g.Vec3d(0.0, 1.0, 0.0),
    g.ImageSpacing2D(1.0, 1.0),
    g.ImageSize2D(512, 512),
)
world = plane.world_from_index(g.ImagePoint2D(20.0, 30.0))
```

## Overlay Checks

Overlay checks are O(1) preflight checks over already-built geometry objects.
They do not walk DICOM datasets and do not allocate transform buffers.

```python
check = dicom.geometry.check_overlay_compatibility(
    source_for_uid, seg_plane, target_for_uid, image_volume
)
if check.can_direct_overlay:
    pass
elif check.can_transform and check.requires_resampling:
    pass
```

`can_transform` normally means the two objects share a usable frame of reference.
When `OverlayCheckOptions.require_same_grid` is true, it is only true for direct
same-grid overlays. `can_direct_overlay` means the grids line up closely enough
for direct index copy. A true `requires_resampling` is not an error; it means
the physical extents overlap and spacing/orientation/grid mapping requires
interpolation or resampling. A pure extent difference with the same grid is
reported as `different_extent`; callers can crop, pad, or clip without
resampling.

## Slice Stack Planning

`analyze_slice_stack()` accepts either a list of `SliceStackInput` records or a
classic list of `DataSet`/`DicomFile` objects. It reports sorted slices, spacing
gaps, uniform spacing, uniform runs, and structured issues. For non-uniform
stacks, `uniform_runs` identifies half-open sorted-slice ranges of at least
three slices that still share one spacing.

`plan_slice_stack()` succeeds only for a uniform rectilinear stack. When it
succeeds, `volume_geometry` describes the output grid and `placements` tells the
caller which decoded input frame belongs at each target `k`.

`SliceStackOptions` keeps general geometry tolerances in `tolerance`, but uses
`slice_position_tolerance_mm` for duplicate slice-position detection and
`origin_residual_tolerance_mm` for in-plane origin drift. Duplicate positions
are rejected by default; `allow_duplicate_positions` permits analysis to
continue but does not turn a zero-gap stack into a uniform volume plan.

```python
inputs = [
    g.SliceStackInput(plane0, "1.2.3", source_index=0, frame_index=0),
    g.SliceStackInput(plane1, "1.2.3", source_index=1, frame_index=0),
]
plan = g.plan_slice_stack(inputs)
if plan.ok:
    for item in plan.placements:
        # Decode sources[item.source_index], then store at item.target_k.
        pass
```

For enhanced multi-frame objects, use `analyze_image_frame_stacks()` first when
the file may contain multiple stacks, time points, phases, or echoes. The
single-stack convenience functions `analyze_image_frame_stack()` and
`plan_image_frame_stack()` succeed only when the file can be interpreted as one
stack or when explicit frame indices are supplied.

For Nuclear Medicine Image Storage, use `analyze_nm_frame_stack()` and
`plan_nm_frame_stack()` instead of the generic enhanced helpers. The MVP NM
adapter is intentionally narrow: `ImageType` value 3 must be `RECON TOMO` or
`RECON GATED TOMO`, `FrameIncrementPointer` must contain exactly one value,
`SliceVector`, and
`SliceVector` is expanded into per-frame planes using the root image plane
normal plus `SpacingBetweenSlices` or, when absent, `SliceThickness`. Projection
`TOMO` / `GATED TOMO` acquisitions and NM organizations based on additional vectors
are rejected rather than treated as regular reconstructed slice stacks.

## Limits

- Non-uniform and non-rectilinear stacks are reported, not forced into
  `ImageVolumeGeometry`.
- `VolumetricProperties=SAMPLED` and `DISTORTED` are preserved by
  `frame_geometry_from_multiframe_image()` but rejected by direct plane helpers.
- NM Image Storage has NM-specific frame organization and is not treated as a
  generic enhanced stack; only the reconstructed TOMO SliceVector adapter is
  currently implemented.
