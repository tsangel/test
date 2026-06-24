# DICOM Segmentation (SEG) 与 Geometry

当你需要查看 DICOM Segmentation (SEG) 元数据、解码 SEG frame、构建 image-plane geometry、规划 slice stack，或检查 mask 与 image 是否可以安全叠加时，可以使用这些 API。

在 DICOM 文件中，这类对象使用 `Modality (0008,0060) = SEG`。更精确的 storage 标识符是 SOP Class：BINARY/FRACTIONAL SEG 使用 Segmentation Storage，LABELMAP SEG 使用 Label Map Segmentation Storage。

下面的示例使用 Python，并接在普通的 `dicom.read_file()` 工作流之后。Geometry layer 不负责构建 viewer、分配最终输出 volume、选择 dominant grid，也不会 resample mask。

mask 示例需要 NumPy 支持。

```bash
pip install "dicomsdl[numpy]"
```

输出值会随输入文件而变化。下面的 SEG 输出摘自一个 binary FDG/FBB brain SEG sample。

## 打开 DICOM Segmentation (SEG) 文件

DICOM Segmentation Storage 和 Label Map Segmentation Storage 都使用 `dicom.seg.read_file()` 打开。它返回的是 `Segmentation` 对象，而不是普通的 `DicomFile`。

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

示例输出：

```text
True
SegmentationType.binary
SegmentationFractionalType.none
None
1.3.6.1.4.1.43046.3.380371456.2303.1779756601.801016
256 256 97 2885
```

如果你已经有字节数据，可以使用 `read_bytes()`。

```python
data = seg_path.read_bytes()
seg = dicom.seg.read_bytes(data, copy=False)
```

Python 中的 SEG 输入入口是 `read_file()` 和 `read_bytes()`。没有 `dicom.seg.from_dicomfile(df)`，因为从已有 Python `DicomFile` 生成 SEG 对象需要复制大数据集并重新解析。

DicomSDL 通过 Segmentation Storage 支持 BINARY/FRACTIONAL SEG，通过 Label Map Segmentation Storage 支持 LABELMAP SEG。SEG adapter 在打开文件时只索引 metadata，不会预先扫描全部 PixelData。LABELMAP stored label value 会在 frame decode/presence scan 时验证，也可以通过显式调用 `validate_label_values()` 验证。

## 查看 Segment

DICOM SEG 中的 `SegmentSequence` 描述该对象包含的 label。每个 item 都是一个语义类别，例如脑结构、肿瘤、器官或派生 mask class。

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

输出片段：

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

也可以按 DICOM segment number 查找。

```python
left_white_matter = seg.segment_by_number(1)
if left_white_matter is not None:
    print(left_white_matter.label)
```

示例输出：

```text
Left-Cerebral-White-Matter
```

Segment number 不是 Python list index。把 frame 与 label 对应起来时，优先使用 `segment.number`。

## 查看 SEG frame

SEG Pixel Data 是 multi-frame。对于 BINARY/FRACTIONAL SEG，每个已存储的 frame 属于一个 referenced segment number。一个 segment 通常有多个 frame，常见情况是每个 frame 对应一张 slice 的 mask。

```python
frame = seg.frames[0]

print(frame.index)
print(frame.referenced_segment_number)
print(frame.image_position_patient)
print(frame.image_orientation_patient)
print(frame.pixel_spacing)
print(frame.slice_thickness)
```

示例输出：

```text
0
1
(-128.000061, -131.25, -38.999939)
(1.0, 0.0, 0.0, 0.0, 1.0, 0.0)
(1.0, 1.0)
1.0
```

如果要编写同时适用于 LABELMAP 的代码，请使用 `present_segment_numbers()`。对于 BINARY/FRACTIONAL SEG，它返回声明的 `ReferencedSegmentNumber`；对于 LABELMAP SEG，它返回该 frame 中实际出现的 non-background label value。`referenced_segment_number` 是兼容用 accessor，在 LABELMAP frame 中没有定义。

```python
print(frame.present_segment_numbers())
```

BINARY frame 的示例输出：

```text
(1,)
```

Frame 也可能包含 source image reference。

```python
for ref in frame.source_images:
    print(ref.sop_class_uid)
    print(ref.sop_instance_uid)
    print(ref.referenced_frame_numbers)
```

示例输出：

```text
1.2.840.10008.5.1.4.1.1.2
1.2.840.113619.2.80.981715802.8664.151072595.1914331.90
[]
```

Source image reference 是来源元数据。它说明 SEG 是由哪些 image 生成的，但并不表示 overlay target 必须是这些 image。做 overlay 时，应先比较 `FrameOfReferenceUID`。

## 解码 BINARY SEG mask

BINARY SEG 在 DICOM 中以 native 1-bit pixel 存储。DicomSDL 会 unpack，并返回值为 `0` 或 `1` 的 `uint8` mask。

```python
mask = seg.to_array(0)
print(mask.shape, mask.dtype)
print(mask.min(), mask.max())
```

示例输出：

```text
(256, 256) uint8
0 1
```

解码某个 segment 的所有 frame：

```python
masks_for_segment = []

for frame in seg.frames_for_segment(1):
    mask = frame.to_array()
    masks_for_segment.append((frame.index, mask))

print("decoded frames:", len(masks_for_segment))
```

示例输出：

```text
decoded frames: 87
```

如果想复用输出数组，可以使用 `decode_frame_into()`。

```python
import numpy as np

out = np.empty((seg.rows, seg.columns), dtype=np.uint8)
returned = seg.decode_frame_into(0, out)
print(returned is out)
print(out.shape, out.dtype, out.min(), out.max())
```

示例输出：

```text
True
(256, 256) uint8 0 1
```

## 解码 FRACTIONAL SEG mask

FRACTIONAL SEG 存储 8-bit raw sample。DicomSDL 返回这些 raw sample，scaling 由调用方明确完成。

```python
import numpy as np

if seg.segmentation_type is dicom.seg.SegmentationType.fractional:
    if not seg.maximum_fractional_value:
        raise ValueError("MaximumFractionalValue is missing")

    raw = seg.to_array(0)  # dtype uint8
    values = raw.astype(np.float32) / float(seg.maximum_fractional_value)
    print(values.min(), values.max())
```

上面的 sample 是 BINARY SEG，因此这个 FRACTIONAL block 对该文件不会打印任何内容。如果 FRACTIONAL SEG 的 raw 值覆盖完整存储范围，输出会类似下面这样：

```text
0.0 1.0
```

这样可以让 probability、occupancy、thresholded boolean mask 等后续流程明确选择所需的精度和数据布局。

## 解码 LABELMAP SEG frame

LABELMAP SEG 会把 label value 直接存储在 PixelData 中。Label value `0` 表示 background，非零值对应 `SegmentSequence` 中的 segment number。DicomSDL 会保留存储表示：8-bit label map 解码为 `uint8`，16-bit label map 解码为 native-endian `uint16`。

```python
if seg.segmentation_type is dicom.seg.SegmentationType.labelmap:
    labels = seg.to_array(0)
    print(labels.shape, labels.dtype)
    print(seg.present_segment_numbers(0))
```

示例输出：

```text
(512, 512) uint16
(1, 24, 300)
```

Palette lookup、color mapping、opacity 和 legend rendering 是 viewer/UI layer 的职责。DicomSDL 返回 stored label sample 和 metadata，不渲染 palette image。

如果需要某个 segment 的 semantic mask，请使用 `mask_for_segment()`。这个 API 在 BINARY、FRACTIONAL 和 LABELMAP SEG 中通用。对于 FRACTIONAL SEG，threshold 使用 normalized `[0, 1]` 单位。

```python
segment_number = 24
mask = seg.mask_for_segment(0, segment_number)
print(mask.shape, mask.dtype, mask.min(), mask.max())
```

示例输出：

```text
(512, 512) uint8 0 1
```

`present_segment_numbers(frame)` 只扫描请求的 LABELMAP frame，并缓存结果。`frames_for_segment(segment_number)` 和 `validate_label_values()` 第一次调用时可能扫描全部 LABELMAP frame；在大型 multi-frame SEG 中，应把它们视为显式 validation/indexing 操作。

## 将 DICOM plane tag 转为 geometry

Geometry module 会把 DICOM image plane 元数据转换成经过验证的 geometry 对象。

```python
g = dicom.geometry

image = dicom.read_file(r"C:\data\CT-slice-0035.dcm")
plane = g.plane_from_single_frame_image(image)

world = plane.world_from_index(g.ImagePoint2D(100.0, 120.0))
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

## 检查 SEG/Image overlay compatibility

Overlay check 使用已经构建好的 geometry 与 frame-of-reference UID。它不会再次遍历数据集，因此可以低成本地重复调用。

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

同一 grid 上的 SEG frame 示例输出：

```text
OverlayCompatibility.compatible
can_transform: True
can_direct_overlay: True
requires_resampling: False
overlaps_extent: True
target_k_range: 35 36
```

这些字段可以这样理解：

- `can_direct_overlay`：grid 足够一致，可以直接按 index copy。
- `requires_resampling`：同一 frame of reference 可用，但 grid mapping 需要 interpolation 或 resampling。
- `different_extent`：grid 一致，但一侧 extent 更小或更大；通常交给 crop、pad 或 clip 策略处理。
- `different_frame_of_reference`：没有外部 registration 时不要 overlay。

当 `can_transform` 为 true 时，先构建一次 transform，再在 paint 或 sampling loop 中复用。

```python
if check.can_transform:
    transform = g.make_plane_to_volume_transform(seg_plane, target_volume)
    center = g.ImagePoint2D(seg.columns / 2.0, seg.rows / 2.0)
    print(transform.target_index_from_source_index(center))
```

示例输出：

```text
ImagePoint3D(i=128, j=128, k=35)
```

## 处理 Enhanced multi-frame image

Enhanced CT/MR/PET 可以在一个文件中存储许多 frame。一个文件中可能混有多个 stack、time point、phase 或 echo，因此应先对 frame 分组。

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
single_plan = g.plan_image_frame_stack(file)
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

- `missing_geometry`：无法解析必要的 plane tag。
- `missing_dimension_module`：缺少 enhanced grouping metadata。
- `multiple_frame_stacks`：面向整个文件的 convenience call 发现了多个 stack。
- `duplicate_slice_position`：两个 frame 位于同一 slice position。
- `non_uniform_spacing`：stack 可以分析，但不是单一 uniform volume grid。
- `inconsistent_slice_origin`：slice origin 出现 in-plane drift，不能用一个 rectilinear affine volume 表达。

即使输入不是 uniform，`uniform_runs` 也能指出有用的连续子范围。DicomSDL 会报告这些范围，但不会选择 dominant grid，也不会 resample 到该 grid。

## 为测试构建 geometry

测试 overlay 的坐标计算不一定需要真实 DICOM 文件。你可以直接构建 geometry 对象。

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

预期输出：

```text
OverlayCompatibility.compatible 4 5
ImagePoint3D(i=10, j=20, k=4)
```

这样可以在 test suite 中不携带真实 patient data 的情况下，对使用 DicomSDL geometry 的应用代码做 unit test。
