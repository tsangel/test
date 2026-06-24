# DICOM Geometry Helpers

このページは `dicom_geometry.h` と Python `dicom.geometry` module の最初の
public contract をまとめる。geometry layer は viewer、viewport、pixel buffer、
resampling policy から分離する。

## Scope

- validated 2D image plane geometry と rectilinear 3D volume geometry を作る。
- classic single-frame image の root DICOM tag から geometry を解決する。
- enhanced multi-frame image では
  `PerFrameFunctionalGroupsSequence -> SharedFunctionalGroupsSequence -> root`
  の順に geometry を解決する。
- DICOM SEG frame geometry を解決する。SEG では root image-plane tag への
  silent fallback は行わない。
- 既に作られた geometry object 同士で overlay compatibility を確認する。
- uniform slice stack から volume geometry と placement list を作る。

この layer は pixel decode、output volume allocation、non-uniform data の
dominant grid 選択、resampling は行わない。

## Coordinate Contract

DicomSDL は image index 名として `i`, `j`, `k` を使う。

- `i`: column 側の image index
- `j`: row 側の image index
- `k`: rectilinear volume の slice/frame index

DICOM `ImageOrientationPatient` の最初の triplet は DicomSDL
`direction_i`、2 番目の triplet は `direction_j` に対応する。DICOM
`PixelSpacing = [row_spacing, column_spacing]` は
`spacing_j = row_spacing`, `spacing_i = column_spacing` に対応する。

`ImagePlaneGeometry::index_to_world_matrix()` は `(i, j, normal_mm, 1)` を
patient/world space に埋め込む。inverse は `(i, j,
signed_normal_distance_mm, 1)` を返す。

## C++ Usage

```cpp
#include <dicom.h>
#include <dicom_geometry.h>

auto file = dicom::read_file(path);
auto plane = dicom::geometry::plane_from_single_frame_image(*file);
if (!plane.ok()) {
    // Inspect plane.status(), plane.tag(), plane.message().
}

auto world = plane.value().world_from_index({12.0, 8.0});
auto index = plane.value().index_from_world(world);
```

Enhanced multi-frame image では frame ごとに geometry を読める。

```cpp
dicom::geometry::FrameGeometryReader reader(*file);
auto frame0 = reader.plane(0);
```

少数の access では convenience function を使い、多数の frame を読む場合は
`FrameGeometryReader` を再利用する。`plane()` は regular slice plane のみを
返し、`SAMPLED`/`DISTORTED` frame geometry は拒否する。frame kind を調べる
caller は `image_frame_geometry()` を使う。

## Python Usage

Python では同じ概念を `dicom.geometry` で公開する。Python surface では
`GeometryBuildResult<T>` を直接出さず、失敗時は `ValueError` を投げる。

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

Overlay check は既に構築済みの geometry object に対する O(1) preflight である。
DICOM dataset を再走査せず、transform buffer も作らない。

```python
check = dicom.geometry.check_overlay_compatibility(
    source_for_uid, seg_plane, target_for_uid, image_volume
)
if check.can_direct_overlay:
    pass
elif check.can_transform and check.requires_resampling:
    pass
```

`can_transform` は通常、2 つの object が利用可能な同じ frame of reference に
あることを表す。ただし `OverlayCheckOptions.require_same_grid` が true の場合は、
direct overlay 可能な同一 grid のときだけ true になる。`can_direct_overlay` は
index copy できる程度に grid が一致することを表す。`requires_resampling` は
error ではなく、physical extent が重なり、spacing/orientation/grid mapping のため
interpolation または resampling が必要な状態である。grid が同じで extent だけが
異なる場合は `different_extent` として報告され、caller は crop/pad/clip で処理できる。

## Slice Stack Planning

`analyze_slice_stack()` は `SliceStackInput` list、または classic
`DataSet`/`DicomFile` list を受け取る。結果には sorted slice、spacing gap、
uniform spacing、structured issue が入る。

`plan_slice_stack()` は uniform rectilinear stack の場合だけ成功する。成功時、
`volume_geometry` は output grid を表し、`placements` はどの input frame を
target `k` に置くかを表す。

`SliceStackOptions` では一般的な geometry tolerance は `tolerance` に置くが、
duplicate slice position の判定には `slice_position_tolerance_mm`、in-plane
origin drift の判定には `origin_residual_tolerance_mm` を使う。duplicate
position は既定では失敗であり、`allow_duplicate_positions` は analysis の継続を
許すだけで、zero-gap stack を uniform volume plan にはしない。

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

Enhanced multi-frame object が複数 stack、time point、phase、echo を含む可能性が
ある場合は、まず `analyze_image_frame_stacks()` を使う。single-stack convenience
function の `analyze_image_frame_stack()` と `plan_image_frame_stack()` は、file が
1 stack として解釈できる場合、または明示的な frame index list が渡された場合に
成功する。

## Limits

- Non-uniform / non-rectilinear stack は `ImageVolumeGeometry` に強制変換せず、
  issue として報告する。
- `VolumetricProperties=SAMPLED` と `DISTORTED` は
  `frame_geometry_from_multiframe_image()` では保持するが、direct plane helper では
  reject する。
- NM Image Storage は NM-specific frame organization を持つため、generic enhanced
  stack として扱わない。
