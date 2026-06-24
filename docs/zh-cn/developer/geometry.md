# DICOM Geometry Helpers

本文记录 `dicom_geometry.h` 与 Python `dicom.geometry` module 的第一版 public
contract。geometry layer 有意与 viewer、viewport、pixel buffer 和 resampling
policy 分离。

## Scope

- 构造经过验证的 2D image plane geometry 和 rectilinear 3D volume geometry。
- 从 classic single-frame image 的 root DICOM tags 解析 geometry。
- 对 enhanced multi-frame image，按
  `PerFrameFunctionalGroupsSequence -> SharedFunctionalGroupsSequence -> root`
  顺序解析 geometry。
- 解析 DICOM SEG frame geometry。SEG 不会静默 fallback 到 root image-plane tags。
- 对已经构造好的 geometry object 做 overlay compatibility 检查。
- 将 uniform slice stack 规划为 volume geometry 和 placement list。

该 layer 不负责 pixel decode、output volume allocation、non-uniform data 的
dominant grid 选择，也不做 resampling。

## Coordinate Contract

DicomSDL 使用 `i`, `j`, `k` 作为 image index 名称：

- `i`: column 方向的 image index
- `j`: row 方向的 image index
- `k`: rectilinear volume 的 slice/frame index

DICOM `ImageOrientationPatient` 的第一个 triplet 映射到 DicomSDL
`direction_i`，第二个 triplet 映射到 `direction_j`。DICOM
`PixelSpacing = [row_spacing, column_spacing]` 映射为
`spacing_j = row_spacing` 与 `spacing_i = column_spacing`。

`ImagePlaneGeometry::index_to_world_matrix()` 将 `(i, j, normal_mm, 1)` 嵌入
patient/world space。其 inverse 返回 `(i, j, signed_normal_distance_mm, 1)`。

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

Enhanced multi-frame image 可以逐 frame 读取 geometry：

```cpp
dicom::geometry::FrameGeometryReader reader(*file);
auto frame0 = reader.plane(0);
```

偶发访问可以使用 convenience function；遍历大量 frame 时应复用
`FrameGeometryReader`。`plane()` 只返回 regular slice plane，并拒绝
`SAMPLED`/`DISTORTED` frame geometry；需要检查 frame kind 的 caller 应使用
`image_frame_geometry()`。

## Python Usage

Python 通过 `dicom.geometry` 暴露同样的概念。Python surface 不直接暴露
`GeometryBuildResult<T>`；失败时抛出 `ValueError`。

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

Overlay check 是对已构造 geometry object 的 O(1) preflight。它不会重新遍历 DICOM
dataset，也不会分配 transform buffer。

```python
check = dicom.geometry.check_overlay_compatibility(
    source_for_uid, seg_plane, target_for_uid, image_volume
)
if check.can_direct_overlay:
    pass
elif check.can_transform and check.requires_resampling:
    pass
```

`can_transform` 通常表示两个 object 位于可用的同一 frame of reference 中。
但当 `OverlayCheckOptions.require_same_grid` 为 true 时，只有可 direct overlay
的同一 grid 才会为 true。`can_direct_overlay` 表示 grid 足够一致，可以直接按
index copy。`requires_resampling` 不是错误；它表示 physical extent 有重叠，
并且 spacing/orientation/grid mapping 需要 interpolation 或 resampling。若 grid
相同而只有 extent 不同，则报告为 `different_extent`，caller 可以用 crop/pad/clip
处理，不需要 resampling。

## Slice Stack Planning

`analyze_slice_stack()` 接受 `SliceStackInput` list，或 classic
`DataSet`/`DicomFile` list。结果包含 sorted slices、spacing gaps、uniform
spacing 和 structured issues。

`plan_slice_stack()` 只在 uniform rectilinear stack 上成功。成功时，
`volume_geometry` 描述 output grid，`placements` 告诉 caller 哪个 input frame 应放到
target `k`。

`SliceStackOptions` 中的一般 geometry tolerance 保存在 `tolerance` 中，但
duplicate slice position 使用 `slice_position_tolerance_mm`，in-plane origin
drift 使用 `origin_residual_tolerance_mm`。duplicate position 默认失败；
`allow_duplicate_positions` 只允许 analysis 继续，不会把 zero-gap stack 变成
uniform volume plan。

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

如果 enhanced multi-frame object 可能包含多个 stack、time point、phase 或 echo，
先调用 `analyze_image_frame_stacks()`。single-stack convenience functions
`analyze_image_frame_stack()` 和 `plan_image_frame_stack()` 只在文件能被解释为一个
stack，或显式传入 frame index list 时成功。

## Limits

- Non-uniform / non-rectilinear stack 不会被强行转换为 `ImageVolumeGeometry`，而是以
  issue 形式报告。
- `VolumetricProperties=SAMPLED` 和 `DISTORTED` 会被
  `frame_geometry_from_multiframe_image()` 保留，但 direct plane helper 会拒绝。
- NM Image Storage 具有 NM-specific frame organization，不作为 generic enhanced
  stack 处理。
