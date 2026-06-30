# DICOM Segmentation (SEG)

当你需要查看 DICOM Segmentation (SEG) metadata、解码 SEG frame，或从 BINARY/FRACTIONAL/LABELMAP SEG 生成 semantic mask 时，请使用本指南。

在 DICOM file 中，这些对象使用 `Modality (0008,0060) = SEG`。更精确的 storage identifier 是 SOP Class：BINARY/FRACTIONAL SEG 使用 Segmentation Storage，LABELMAP SEG 使用 Label Map Segmentation Storage。

mask 示例需要安装 NumPy support:

```bash
pip install "dicomsdl[numpy]"
```

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
print(seg.segments_overlap)
print(seg.maximum_fractional_value)
print(seg.frame_of_reference_uid)
print(seg.rows, seg.columns, seg.segment_count, seg.frame_count)
```

示例输出：

```text
True
SegmentationType.binary
SegmentationFractionalType.none
SegmentsOverlap.undefined
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

DicomSDL 通过 Segmentation Storage 支持 BINARY/FRACTIONAL SEG，通过 Label Map Segmentation Storage 支持 LABELMAP SEG。SEG 适配器在打开文件时只索引元数据，不会预先扫描整个 PixelData 元素。LABELMAP 中存储的 label value 会在解码 frame 或执行 presence scan 时验证，也可以通过显式调用 `validate_label_values()` 验证。

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

## 按 SegmentationType 选择 API

`seg.segmentation_type` 表示 PixelData 的存储形式。如果一段代码要同时处理 BINARY、FRACTIONAL 和 LABELMAP SEG，请按下面的差异选择 API。

| SegmentationType | `to_array()` / `decode_frame()` | segment 归属 | 推荐方式 |
| --- | --- | --- | --- |
| `binary` | `uint8` mask 值 `0` 或 `1` | 每个 frame 有一个 `ReferencedSegmentNumber` | 按 segment 遍历时使用 `frames_for_segment()`。`to_array()` 的结果已经是该 frame 的语义 mask。 |
| `fractional` | 存储的 raw `uint8` sample | 每个 frame 有一个 `ReferencedSegmentNumber` | threshold mask 使用 `mask_for_segment(..., fractional_threshold=...)`；如果需要 raw probability/occupancy value，则用 `MaximumFractionalValue` 自行 scaling。 |
| `labelmap` | 存储的 label value，`uint8` 或 native-endian `uint16` | 一个 frame 可以包含多个 segment number | 使用 `present_segment_numbers()` 和 `mask_for_segment()`。不要在 LABELMAP frame 中使用 `referenced_segment_number`。 |

最稳妥的通用模式如下：

```python
for frame in seg.frames:
    for segment_number in frame.present_segment_numbers():
        mask = frame.mask_for_segment(segment_number)
        # 将 mask 与 frame.image_position_patient / geometry mapping 一起使用。
```

如果需要存储的 pixel 表示，请使用 `to_array()`。如果需要与存储类型无关的 `uint8` 0/1 segment mask，请使用 `mask_for_segment()`。对于 LABELMAP，`frames_for_segment()` 和 `validate_label_values()` 第一次调用时可能扫描全部 frame；对于 BINARY/FRACTIONAL，它们使用打开文件时建立的元数据索引。

## 解码 BINARY SEG mask

BINARY SEG 在 DICOM 中以 1-bit pixel 存储。DicomSDL 会 unpack，并返回值为 `0` 或 `1` 的 `uint8` mask。Native uncompressed 和 Encapsulated Uncompressed BINARY SEG 都可以这样读取。C++ 调用方如果想避免 unpack 整个 frame，可以使用 `binary_frame_bits()` 和 `for_each_binary_frame_set_bit()` 只遍历 set bit。

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

## 将 BINARY SEG frame 打包成一个 label volume

这个功能最适合包含大量、且很少互相 overlap 的 BINARY segment 的 SEG。在构建 viewer
这类 application 时，如果为这种 SEG 的每个 segment 创建一个 3D mask texture，会浪费
memory 和 GPU binding work。DicomSDL 可以把这些 BINARY SEG frame 打包成一个
`uint16` label volume。Target grid 的选择，以及每个 SEG frame 放到哪个 slice，由
application 决定。

```python
import numpy as np

# 由 application 的 image geometry / stack 代码生成。
slice_count = 127
slice_index_by_frame = {...}  # frame_index -> slice_index

frame_placements = [
    (frame.index, slice_index_by_frame[frame.index])
    for frame in seg.frames
]

packed = seg.build_binary_label_volume(frame_placements, slices=slice_count)
labels = packed.label_volume

print(labels.shape, labels.dtype)
print(packed.source_dicom_segment_by_label_id[:4])
```

示例输出：

```text
(127, 256, 256) uint16
[0, 1, 2, 3]
```

`labels` 的 shape 是 `(slices, rows, columns)`。Code `0` 表示 background。
其他值是 runtime label code，其中某些 code 可能表示两个或更多 segment 的 overlap。
需要 semantic membership 时，请使用返回的 `BinaryLabelVolume` 对象上的 table API。

```python
segment_number = 2
label_id = packed.label_id_for_segment_number(segment_number)
mask = packed.restore_mask_for_segment(segment_number)

code = int(labels[30, 120, 90])
print(packed.label_set(code))
```

`BinaryLabelVolume` 在构建 volume 时也会保存每个 label 的 voxel 统计。
坐标使用 voxel index 的 `(x, y, z)` 顺序，其中 `x` 是 column，`y` 是 row，
`z` 是 slice。

```python
stats = packed.label_stats_for_segment(segment_number)
print(stats.voxel_count)
print(stats.min_index, stats.max_index)
print(stats.centroid_index)
```

需要显示 LUT 时，传入 dense 的 `label_id -> RGBA` table。返回数组的 shape 是
`(label_code_count, 4)`，可按存储的 label code 直接索引。Overlap label code 的
颜色由其包含的 label RGBA 平均得到；如果 application 需要其他 blend policy，可以
通过 `label_set()` 读取 label 组成并自行构建 LUT。Renderer 如果需要 2D texture，
padding 或 reshape 应由 application 决定。

```python
label_rgba_by_label_id = [
    (0, 0, 0, 0),       # label_id 0: background
    (255, 64, 64, 96),  # label_id 1
    (64, 192, 255, 96), # label_id 2
]

rgba_lut = packed.build_rgba8_lut(
    label_rgba_by_label_id,
    background=(0, 0, 0, 0),
)
assert rgba_lut.shape == (packed.label_code_count, 4)
```

如果 viewer pipeline 已经拥有 staging buffer，可以使用 allocation-free variant。

```python
label_volume = np.empty((slice_count, seg.rows, seg.columns), dtype=np.uint16)

packed = seg.build_binary_label_volume_into(
    label_volume,
    frame_placements,
    slices=slice_count,
)

assert np.shares_memory(packed.label_volume, label_volume)
```

当 application 想直接管理 CPU/GPU staging allocation 时，推荐使用
`build_binary_label_volume_into()`。Visibility、color、opacity 和 lookup-table
rendering 仍然由 viewer 负责。

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

LABELMAP SEG 会把 label value 直接存储在 PixelData 中。存储值对应 `SegmentSequence` 中的 segment number。存在 `PixelPaddingValue` 时，该 segment number 会被视为 background（背景），并从 `present_segment_numbers()` 中排除。DicomSDL 会保留原始存储表示：8-bit label map 解码为 `uint8`，16-bit label map 解码为 native-endian `uint16`。

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

Palette lookup、color mapping、opacity 和 legend rendering 是 viewer/UI layer 的职责。DicomSDL 返回存储的 label sample 和元数据，不渲染 palette image。

如果需要某个 segment 的语义 mask，请使用 `mask_for_segment()`。这个 API 在 BINARY、FRACTIONAL 和 LABELMAP SEG 中通用。对于 FRACTIONAL SEG，threshold 使用归一化的 `[0, 1]` 单位。

```python
segment_number = 24
mask = seg.mask_for_segment(0, segment_number)
print(mask.shape, mask.dtype, mask.min(), mask.max())
```

示例输出：

```text
(512, 512) uint8 0 1
```

`present_segment_numbers(frame)` 只扫描请求的 LABELMAP frame，并缓存结果。`frames_for_segment(segment_number)` 第一次调用时可能扫描所有 LABELMAP frame。`validate_label_values()` 会验证所有 LABELMAP label，并 decode FRACTIONAL frame 以检查 `MaximumFractionalValue` 范围；在大型 multi-frame SEG 中，应把它视为显式验证操作。
