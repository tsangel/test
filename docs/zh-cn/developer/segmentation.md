# DICOM Segmentation MVP

本文记录 DicomSDL high-level DICOM SEG adapter 的第一版契约。核心 DICOM 读取仍保留在 `dicom.h` 中，SEG 解释能力通过可选的 public header `dicom_seg.h` 暴露。

## 支持范围

- SOP Class 支持用于 BINARY/FRACTIONAL 的 Segmentation Storage (`1.2.840.10008.5.1.4.1.1.66.4`)，以及用于 LABELMAP 的 Label Map Segmentation Storage (`1.2.840.10008.5.1.4.1.1.66.7`)。
- BINARY SEG 支持 native 1-bit multi-frame PixelData。`decode_frame_into()` 会 unpack 一个已存储的 frame，并以每 pixel 1 byte、值为 `0` 或 `1` 的形式返回。
- FRACTIONAL SEG 支持 native 8-bit PixelData。decode 结果是 raw `uint8` sample，调用方可用 `raw_value / MaximumFractionalValue` 转换为 fractional value。
- LABELMAP SEG 通过 Label Map Segmentation Storage 支持 native uncompressed 8-bit 或 16-bit stored label sample。decode 会保留已存储的 label value；palette lookup 和 color rendering 由 viewer/UI layer 负责。
- metadata view 会按 frame 索引 `SegmentSequence`、`PerFrameFunctionalGroupsSequence`、`SharedFunctionalGroupsSequence`、source image reference 和 `FrameOfReferenceUID`。

## Post-MVP

- 将 frame mask 组装为 3D array 的 volume reconstruction API。
- 将 SEG frame 映射到显示目标 image 的 affine / overlay helper。
- compressed / encapsulated SEG PixelData，包括 RLE 等 encapsulated transfer syntax。

## 必需 Metadata

SEG adapter 默认验证此 MVP 所需的 metadata。

- `FrameOfReferenceUID` 是必需项，也是判断 SEG 能否直接 overlay 到另一幅 image 上的 primary key。`SourceImageSequence` 是 provenance metadata，并不表示只能显示在这些 source image 上。
- `Rows`、`Columns`、`SegmentSequence` 和 `PerFrameFunctionalGroupsSequence` 是必需项。
- `SharedFunctionalGroupsSequence` 必须恰好包含一个 item。
- BINARY/FRACTIONAL frame 必须能解析出一个 `ReferencedSegmentNumber`。
- FRACTIONAL SEG 必须包含 `SegmentationFractionalType` 和 `MaximumFractionalValue`。
- LABELMAP SEG 必须使用 Label Map Segmentation Storage，且满足 `SegmentationType=LABELMAP`、`BitsAllocated` 为 8 或 16、unsigned single-sample pixel、`PhotometricInterpretation` 为 `MONOCHROME2` 或 `PALETTE COLOR`。stored label value 的验证不会在 file open 时执行，而是在 decode / presence query 或调用 `validate_label_values()` 时 lazy 执行。

如果这些条件不满足，adapter 应明确报错，而不是返回可能误导调用方的 mask。

## Pixel 契约

BINARY SEG MVP 支持 native 1-bit DICOM PixelData。public API 返回 decoded 8-bit frame，而不是 packed bits。

```cpp
std::vector<std::uint8_t> mask(seg->rows() * seg->columns());
seg->decode_frame_into(frame_index, mask);
// mask values are 0 or 1
```

FRACTIONAL SEG MVP 返回已存储的 raw 8-bit sample。

```python
raw = seg.to_array(0)  # dtype uint8
fraction = raw.astype("float32") / seg.maximum_fractional_value
```

scaling 由调用方执行，这样 probability / occupancy 的使用方可以自行选择输出 precision 和 memory layout。

对于 LABELMAP SEG，`to_array()` 会保留 stored sample dtype：8-bit label map 返回 `uint8`，16-bit label map 返回 native-endian `uint16`。`present_segment_numbers(frame)` 报告该 frame 中实际出现的 non-background label；存在 `PixelPaddingValue` 时，该 segment number 会被视为 background（背景）并从结果中排除。`mask_for_segment(frame, segment_number)` 返回请求 segment 的 semantic `uint8` 0/1 mask。unknown stored label value 不会在 file open 时检查；它们会在相关 frame 被 decode / scan 时，或调用 `validate_label_values()` 时报告为 error。

## API Pattern

C++ 代码通常使用 SEG convenience reader。已经持有 parsed `DicomFile` 的 advanced caller 可以通过 `from_dicomfile()` 将所有权移交给 SEG adapter。

```cpp
#include <dicom.h>
#include <dicom_seg.h>

auto seg = dicom::seg::read_file(path);

auto file = dicom::read_file(path);
auto seg_from_file = dicom::seg::from_dicomfile(std::move(file));
```

C++ adapter 拥有 `DicomFile`；返回的 segment / frame view 从它借用数据。这样可以避免复制字符串和 DICOM item，同时让 view lifetime 保持简单。

Python 使用相同的 naming。

```python
import dicomsdl as dicom

seg = dicom.seg.read_file(path)
seg = dicom.seg.read_bytes(data, copy=False)
```

Python 不提供 `dicom.seg.from_dicomfile(df)` helper。Python 无法在不复制完整 dataset 的情况下，从已有 `DicomFile` 对象中 move 出 C++ unique ownership；对于大型 SEG，这个成本太容易被误触发。因此 Python API 只提供 `read_file()` 和 `read_bytes()` 入口。

## Regression Tests

repository 中保留 synthetic BINARY/FRACTIONAL/LABELMAP SEG 的 C++ / Python test。实际 sample 可能是 private data，因此通过环境变量启用 optional local regression。

```powershell
$env:DICOMSDL_SEG_SAMPLE_PATH = "C:\path\to\sample-seg.dcm"
python -m pytest tests/python/test_segmentation.py -q
```

Python wheel 通过 `package_data` 包含 stub。CMake target 暴露 repository 的 `include/`，因此使用 source tree 的 build 可以 include `<dicom_seg.h>`。正式 CMake install/export rule 仍在此 MVP 范围之外。
