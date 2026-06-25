# DICOM Geometry

DicomSDL 会从 `ImagePositionPatient`、`ImageOrientationPatient`、
`PixelSpacing`、`Rows`、`Columns` 等 DICOM Image Plane attributes 构建 geometry
object。

`ImagePlaneGeometry` 包含 plane origin、row/column direction、normal、pixel
spacing、image size，以及 index/world transform。`ImageVolumeGeometry`
在此基础上增加 slice direction、slice spacing、slice count 和 3D
index/world transform，用于表示 rectilinear volume geometry。

## 从 Image Plane attributes 构建 geometry

Geometry module 会把 DICOM Image Plane attributes 转换成经过验证的 geometry 对象。

```python
geom = dicom.geometry

image = dicom.read_file(r"C:\data\CT-slice-0035.dcm")
plane = geom.plane_from_single_frame_image(image)

world = plane.world_from_index(geom.ImagePoint2D(100.0, 120.0))
index = plane.index_from_world(world)

print(world)
print(index)
```

对于 1 mm axial slice，示例输出如下：

```text
Point3d(x=-28.000061, y=-11.25, z=-38.999939)
ImagePoint2D(i=100, j=120)
```

DicomSDL 使用以下 image index 名称：

- `i`：column 方向的 image index。
- `j`：row 方向的 image index。
- `k`：volume 的 slice/frame index。

DICOM `PixelSpacing` 的顺序是 `[row_spacing, column_spacing]`。DicomSDL 将其映射为 `spacing_j` 和 `spacing_i`。

```python
print("columns, rows:", plane.columns, plane.rows)
print("spacing_i, spacing_j:", plane.spacing_i, plane.spacing_j)
```

示例输出：

```text
columns, rows: 512 512
spacing_i, spacing_j: 1.0 1.0
```

## 规划 Classic slice stack

Classic CT/MR/PET series 通常每张 slice 对应一个 SOP instance。`plan_slice_stack()` 会按物理 slice 顺序排序这些文件，并返回 volume geometry 与 placement list。

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

示例输出：

```text
256 256 91
1.0 1.0 1.0
source file 0 frame 0 -> k 0
source file 1 frame 0 -> k 1
source file 2 frame 0 -> k 2
source file 3 frame 0 -> k 3
source file 4 frame 0 -> k 4
```

如果要自己组装 pixel volume，可以使用 placement。

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

示例输出：

```text
(91, 256, 256) uint16
```

Geometry layer 不解码 pixel，也不分配输出 volume。这样 viewer、batch job 和 resampling pipeline 可以自行选择 memory layout 与策略。

## 处理 Enhanced multi-frame image

Enhanced CT/MR/PET 可以在一个文件中存储许多 frame。一个文件中可能混有多个 stack、time point、phase 或 echo，因此应先对 frame 分组。

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

单个 enhanced CT stack 的示例输出：

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

`plane_from_multiframe_image(file, frame_index)` 是 direct overlay helper，只返回 regular slice plane。`frame_geometry_from_multiframe_image()` 会保留 `ImageFrameGeometryKind`，因此调用方可以检查 sampled projection 或 distorted frame metadata，而不会误把它们当作普通 slice 处理。

只有在确认整个文件就是一个 stack 时，才直接使用 `plan_image_frame_stack(file)`。如果文件可能包含多个 group，请显式按 group 的 frame list 进行 plan。

```python
single_plan = geom.plan_image_frame_stack(file)
if not single_plan.ok:
    for issue in single_plan.issues:
        print(issue.status, issue.frame_index, issue.tag, issue.message)
```

如果文件包含多个 stack，输出可能类似：

```text
SliceStackStatus.multiple_frame_stacks 0 (0020,9157) file contains multiple frame stacks
```

## 处理 Reconstructed NM TOMO stack

Nuclear Medicine Image Storage 使用较早的 frame organization tags，和 enhanced image 不同。DicomSDL 为 reconstructed TOMO stack 提供了专用 adapter。

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

示例输出：

```text
128 128 64
[(0, 0), (1, 1), (2, 2), (3, 3), (4, 4), (5, 5), (6, 6), (7, 7), (8, 8), (9, 9)]
```

当前 NM adapter 只接受 `RECON TOMO` 和 `RECON GATED TOMO` 中同时满足以下条件的对象：存在 `NumberOfFrames`，且 `FrameIncrementPointer` 正好只有一个值 `SliceVector`。Projection acquisition 或包含 time/energy 等额外 vector 的对象会被拒绝，而不是被静默推断。

## 诊断 stack failure

Analysis 对象会保留结构化 issue，方便 viewer 或 batch script 决定下一步。

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

Non-uniform stack 的示例输出：

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

常见 status：

- `missing_geometry`：无法解析必要的 Image Plane attributes。
- `missing_dimension_module`：缺少 enhanced grouping metadata。
- `multiple_frame_stacks`：面向整个文件的 convenience call 发现了多个 stack。
- `duplicate_slice_position`：两个 frame 位于同一 slice position。
- `non_uniform_spacing`：stack 可以分析，但不是单一 uniform volume grid。
- `inconsistent_slice_origin`：slice origin 出现 in-plane drift，不能用一个 rectilinear affine volume 表达。

即使输入不是 uniform，`uniform_runs` 也能指出有用的连续子范围。DicomSDL 会报告这些范围，但不会选择 dominant grid，也不会 resample 到该 grid。
