# DICOM Segmentation (SEG)

本文记录 DicomSDL high-level DICOM Segmentation adapter 的公开契约。在 DICOM 中，Segmentation 是 SEG modality（`Modality = SEG`）。DicomSDL 将 core dataset 读取保留在 `dicom.h` 中，并通过可选 include 的 public header `dicom_seg.h` 提供 SEG 解释层。

## 支持范围

- SOP Class：BINARY/FRACTIONAL 使用 Segmentation Storage（`1.2.840.10008.5.1.4.1.1.66.4`），LABELMAP 使用 Label Map Segmentation Storage（`1.2.840.10008.5.1.4.1.1.66.7`）。
- BINARY SEG：支持 native 1-bit multi-frame PixelData 的 read/decode。compressed BINARY SEG 的 pixel transcode 暂不支持，直到 core pixel layer 能 end to end 表示 stored `BitsAllocated=1` layout。
- FRACTIONAL SEG：支持 8-bit samples，适用于 native uncompressed、Encapsulated Uncompressed，以及有对应 codec 的 lossless compressed transfer syntax。decode 返回 raw `uint8` samples，caller 可以用 `raw_value / MaximumFractionalValue` 转换。
- LABELMAP SEG：通过 Label Map Segmentation Storage 支持 8-bit 和 16-bit stored label samples，适用于 native uncompressed、Encapsulated Uncompressed，以及有对应 codec 的 lossless compressed transfer syntax。decode 保留 stored label value；palette lookup 和 color rendering 属于 viewer/UI layer 的职责。
- lossy 或 near-lossless compressed SEG source/target 会被拒绝。本契约不支持 Big Endian Label Map SEG。
- metadata view 会按 frame index `SegmentSequence`、`PerFrameFunctionalGroupsSequence`、`SharedFunctionalGroupsSequence`、source image reference 和 `FrameOfReferenceUID`。

## Transfer Syntax 支持

| PixelData storage | BINARY | FRACTIONAL | LABELMAP |
| --- | --- | --- | --- |
| Native uncompressed Little Endian | 支持 read/decode | 支持 read/write/transcode | 支持 read/write/transcode |
| Native Explicit VR Big Endian | BINARY native read 遵循 generic DICOM path | 仅在 generic pixel path 支持的范围内支持 | 不支持 |
| Encapsulated Uncompressed Explicit VR Little Endian | 不支持 BINARY pixel transcode | 支持 read/write/transcode | 支持 read/write/transcode |
| RLE Lossless、JPEG-LS Lossless、JPEG 2000 Lossless、HTJ2K Lossless、JPEG XL Lossless 等已注册 codec 的 lossless compressed image syntax | core 1-bit layout/write support 完成前不支持 | 支持 read/write/transcode | 支持 read/write/transcode |
| Lossy 或 near-lossless compressed syntax | 拒绝 | 拒绝 | 拒绝 |
| 不支持的 compressed/video/referenced source codec | 在 frame decode 或 transcode 时拒绝 | 在 frame decode 或 transcode 时拒绝 | 在 frame decode 或 transcode 时拒绝 |

## 必需 Metadata

SEG adapter 默认验证安全解释 frame 所需的 metadata。

- `FrameOfReferenceUID` 是必需字段，也是判断 SEG 是否可以直接 overlay 到另一幅 image 上的 primary key。`SourceImageSequence` 是 provenance metadata，并不表示只能显示在该 source image 上。
- `Rows`、`Columns`、`SegmentSequence` 和 `PerFrameFunctionalGroupsSequence` 是必需字段。
- `SharedFunctionalGroupsSequence` 必须恰好包含一个 item。
- BINARY/FRACTIONAL frame 必须能解析到一个 `ReferencedSegmentNumber`。
- FRACTIONAL SEG 必须包含 `SegmentationFractionalType` 和 `MaximumFractionalValue`。
- LABELMAP SEG 必须使用 Label Map Segmentation Storage，并具有 `SegmentationType=LABELMAP`、`BitsAllocated` 8 或 16、unsigned single-sample pixels，以及 `PhotometricInterpretation` `MONOCHROME2` 或 `PALETTE COLOR`。Stored label value 不在 file open 时验证，而是在 decode/presence query 或调用 `validate_label_values()` 时 lazy 验证。

如果不满足这些条件，adapter 会明确失败，而不是静默返回可能误导 caller 的 mask。

## Pixel 契约

`to_array()` 和 `decode_frame()` 保留 stored representation。若要在 BINARY、FRACTIONAL、LABELMAP 之间用统一方式取得某个 segment 的 semantic 0/1 mask，请使用 `mask_for_segment()`。

对于 BINARY SEG，`decode_frame_into()` 会 unpack 一个 stored 1-bit frame，并返回 `uint8` 值 `0` 或 `1`。

```cpp
std::vector<std::uint8_t> mask(seg->rows() * seg->columns());
seg->decode_frame_into(frame_index, mask);
// mask values are 0 or 1
```

对于 FRACTIONAL SEG，`to_array()` 返回 raw `uint8` stored samples。

```python
raw = seg.to_array(0)  # dtype uint8
fraction = raw.astype("float32") / seg.maximum_fractional_value
```

`mask_for_segment(..., fractional_threshold=...)` 会生成 semantic binary mask。默认 threshold `0.0` 表示 `sample > 0`；其他 threshold 按 `sample / MaximumFractionalValue >= fractional_threshold` 比较。

对于 LABELMAP SEG，`to_array()` 保留 stored sample dtype：8-bit label map 为 `uint8`，16-bit label map 为 native-endian `uint16`。`decode_frame()` 返回 native typed sample bytes，而不是 raw PixelData byte order。`present_segment_numbers(frame)` 报告该 frame 中实际出现的 non-background labels；如果存在 `PixelPaddingValue`，该 segment number 会被视为 background 并从结果中排除。`mask_for_segment(frame, segment_number)` 返回请求 segment 的 semantic `uint8` 0/1 mask。Unknown stored label value 不在 file open 时检查；它会在相关 frame 被 decode/scan 时，或调用 `validate_label_values()` 时报告。

LABELMAP presence cache 是 lazy 且 thread-safe 的。多个 thread 同时首次查询同一 frame 的 presence 时，frame-local scan 可能重复执行；但 ready cache entry 和 all-frame index 是 immutable 的，不会被替换。All-frame index 的构建是 serialized 的。

`referenced_segment_number` 仍作为 BINARY/FRACTIONAL 的 compatibility accessor 保留。LABELMAP frame 可以包含多个 segment labels，因此在 LABELMAP 上调用该 accessor 会抛出 error。通用代码应使用 `present_segment_numbers()` 和 `mask_for_segment()`。

如果 decode 或 validation 抛出异常，`_into()` API 可能会让 output buffer 处于 partial write 状态。

## API 模式

C++ 通常使用 SEG convenience readers。已经持有 parsed `DicomFile` 的 advanced caller 可以用 `from_dicomfile()` 将 ownership 移交给 SEG adapter。

```cpp
#include <dicom.h>
#include <dicom_seg.h>

auto seg = dicom::seg::read_file(path);

auto file = dicom::read_file(path);
auto seg_from_file = dicom::seg::from_dicomfile(std::move(file));
```

C++ adapter 拥有 `DicomFile`，返回的 segment/frame views 会 borrow 其中的 dataset。这样可以避免复制 strings 和 DICOM items，同时保持 view lifetime 简单。

Python 使用相同命名。

```python
import dicomsdl as dicom

seg = dicom.seg.read_file(path)
seg = dicom.seg.read_bytes(data, copy=False)
```

Python 不提供 `dicom.seg.from_dicomfile(df)` helper。Python 无法从已有 `DicomFile` object 中 move 出 C++ unique ownership；若要模拟这个行为，需要复制或重新解析整个大型 SEG dataset，容易让 caller 无意中选择高成本路径。

## Regression Tests

repository 中保留了 synthetic BINARY、FRACTIONAL、LABELMAP SEG 的 C++/Python tests。也可以在不提交 private data 的情况下启用 local real-sample regression。

```powershell
$env:DICOMSDL_SEG_SAMPLE_PATH = "C:\path\to\sample-seg.dcm"
python -m pytest tests/python/test_segmentation.py -q
```

Python wheel 通过 `package_data` 包含 stub。CMake target 会暴露 repository 的 `include/` directory，因此消费 source tree 的 build 可以使用 `<dicom_seg.h>`。正式的 CMake install/export rules 仍不在本契约范围内。
