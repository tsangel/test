# DICOM Segmentation (SEG)

DICOM Segmentation (SEG) metadata を確認し、SEG frame を decode し、BINARY/FRACTIONAL/LABELMAP SEG から semantic mask を作るときはこのガイドを使います。

DICOM file では、これらの object は `Modality (0008,0060) = SEG` を使います。正確な storage identifier は SOP Class です。BINARY/FRACTIONAL SEG は Segmentation Storage、LABELMAP SEG は Label Map Segmentation Storage を使います。

mask example には NumPy support をインストールします:

```bash
pip install "dicomsdl[numpy]"
```

## DICOM Segmentation (SEG) ファイルを開く

DICOM Segmentation Storage と Label Map Segmentation Storage は `dicom.seg.read_file()` で開きます。戻り値は通常の `DicomFile` ではなく `Segmentation` オブジェクトです。

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

出力例:

```text
True
SegmentationType.binary
SegmentationFractionalType.none
None
1.3.6.1.4.1.43046.3.380371456.2303.1779756601.801016
256 256 97 2885
```

すでにバイト列を持っている場合は `read_bytes()` を使います。

```python
data = seg_path.read_bytes()
seg = dicom.seg.read_bytes(data, copy=False)
```

Python で SEG を読むときは `read_file()` または `read_bytes()` から始めます。`dicom.seg.from_dicomfile(df)` は用意していません。既存の Python `DicomFile` から SEG オブジェクトを作るには、大きなデータセットをコピーして再解析する必要があるためです。

DicomSDL は BINARY/FRACTIONAL SEG を Segmentation Storage 経由で、LABELMAP SEG を Label Map Segmentation Storage 経由でサポートします。SEG アダプターは、開く時点で PixelData 全体をスキャンせず、メタデータだけをインデックス化します。LABELMAP に保存された label value は、frame をデコードする時や presence scan の時点、または `validate_label_values()` を明示的に呼んだ時に検証されます。

## Segment を確認する

DICOM SEG の `SegmentSequence` は、そのオブジェクトに含まれる label を説明します。各 item は 1 つの意味的なクラスで、脳構造、腫瘍、臓器、派生 mask class などに対応します。

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

出力の抜粋:

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

DICOM segment number で直接探すこともできます。

```python
left_white_matter = seg.segment_by_number(1)
if left_white_matter is not None:
    print(left_white_matter.label)
```

出力例:

```text
Left-Cerebral-White-Matter
```

Segment number は Python の list index ではありません。frame と label を対応させるときは `segment.number` を基準にしてください。

## SEG frame を確認する

SEG Pixel Data は multi-frame です。BINARY/FRACTIONAL SEG では、保存された frame は 1 つの referenced segment number に属します。1 つの segment が多くの frame を持ち、各 frame が 1 slice の mask になることがよくあります。

```python
frame = seg.frames[0]

print(frame.index)
print(frame.referenced_segment_number)
print(frame.image_position_patient)
print(frame.image_orientation_patient)
print(frame.pixel_spacing)
print(frame.slice_thickness)
```

出力例:

```text
0
1
(-128.000061, -131.25, -38.999939)
(1.0, 0.0, 0.0, 0.0, 1.0, 0.0)
(1.0, 1.0)
1.0
```

LABELMAP も扱うコードでは `present_segment_numbers()` を使います。BINARY/FRACTIONAL SEG では宣言された `ReferencedSegmentNumber` を返し、LABELMAP SEG ではその frame に実際に存在する non-background label value を返します。`referenced_segment_number` は互換用 accessor で、LABELMAP frame では定義されません。

```python
print(frame.present_segment_numbers())
```

BINARY frame の出力例:

```text
(1,)
```

Frame には source image reference が含まれることがあります。

```python
for ref in frame.source_images:
    print(ref.sop_class_uid)
    print(ref.sop_instance_uid)
    print(ref.referenced_frame_numbers)
```

出力例:

```text
1.2.840.10008.5.1.4.1.1.2
1.2.840.113619.2.80.981715802.8664.151072595.1914331.90
[]
```

Source image reference は、SEG がどの image から作られたかを示す由来 metadata です。ただし、overlay 先が必ずその image でなければならないという意味ではありません。Overlay ではまず `FrameOfReferenceUID` を比較します。

## SegmentationType ごとの API の選び方

`seg.segmentation_type` は PixelData の保存表現を示します。BINARY、FRACTIONAL、LABELMAP SEG を同じコードで扱う場合は、次の違いを基準にしてください。

| SegmentationType | `to_array()` / `decode_frame()` | segment membership | 推奨する使い方 |
| --- | --- | --- | --- |
| `binary` | `uint8` mask 値 `0` または `1` | frame ごとに 1 つの `ReferencedSegmentNumber` | segment 単位の反復処理には `frames_for_segment()` を使います。`to_array()` の結果はその frame の segment mask です。 |
| `fractional` | 保存された raw `uint8` sample | frame ごとに 1 つの `ReferencedSegmentNumber` | threshold mask には `mask_for_segment(..., fractional_threshold=...)` を使い、raw probability/occupancy value が必要な場合は `MaximumFractionalValue` でスケーリングします。 |
| `labelmap` | 保存された label value、`uint8` または native-endian `uint16` | 1 frame に複数の segment number が入ることがあります | `present_segment_numbers()` と `mask_for_segment()` を使います。LABELMAP frame では `referenced_segment_number` を使いません。 |

最も安全な共通パターンは次の形です。

```python
for frame in seg.frames:
    for segment_number in frame.present_segment_numbers():
        mask = frame.mask_for_segment(segment_number)
        # frame.image_position_patient / geometry mapping と一緒に mask を使います。
```

保存された pixel 表現が必要な場合は `to_array()` を使います。保存形式に依存しない `uint8` 0/1 の segment mask が必要な場合は `mask_for_segment()` を使います。LABELMAP では `frames_for_segment()` と `validate_label_values()` が初回呼び出し時に全 frame をスキャンすることがあります。BINARY/FRACTIONAL では、開く時点で作成したメタデータインデックスを使います。

## BINARY SEG mask を decode する

BINARY SEG は native 1-bit pixel として DICOM に保存されます。DicomSDL はこれを unpack し、値が `0` または `1` の `uint8` mask を返します。

```python
mask = seg.to_array(0)
print(mask.shape, mask.dtype)
print(mask.min(), mask.max())
```

出力例:

```text
(256, 256) uint8
0 1
```

1 つの segment に属する frame をすべて decode します。

```python
masks_for_segment = []

for frame in seg.frames_for_segment(1):
    mask = frame.to_array()
    masks_for_segment.append((frame.index, mask))

print("decoded frames:", len(masks_for_segment))
```

出力例:

```text
decoded frames: 87
```

出力配列を再利用したい場合は `decode_frame_into()` を使います。

```python
import numpy as np

out = np.empty((seg.rows, seg.columns), dtype=np.uint8)
returned = seg.decode_frame_into(0, out)
print(returned is out)
print(out.shape, out.dtype, out.min(), out.max())
```

出力例:

```text
True
(256, 256) uint8 0 1
```

## FRACTIONAL SEG mask を decode する

FRACTIONAL SEG は 8-bit raw sample を保存します。DicomSDL は raw sample をそのまま返し、scaling は呼び出し側で明示的に行います。

```python
import numpy as np

if seg.segmentation_type is dicom.seg.SegmentationType.fractional:
    if not seg.maximum_fractional_value:
        raise ValueError("MaximumFractionalValue is missing")

    raw = seg.to_array(0)  # dtype uint8
    values = raw.astype(np.float32) / float(seg.maximum_fractional_value)
    print(values.min(), values.max())
```

上の sample は BINARY SEG なので、この FRACTIONAL block は何も出力しません。Raw 値が保存範囲全体に広がる FRACTIONAL SEG では、次のような出力になります。

```text
0.0 1.0
```

こうしておくと、probability、occupancy、thresholded boolean mask などの後段処理で、必要な精度とデータ配置を呼び出し側が選べます。

## LABELMAP SEG frame を decode する

LABELMAP SEG は label value を PixelData に直接保存します。保存された値は `SegmentSequence` の segment number に対応します。`PixelPaddingValue` がある場合、その segment number は background（背景）として扱われ、`present_segment_numbers()` からは除外されます。DicomSDL は保存表現をそのまま返します。8-bit label map は `uint8`、16-bit label map は native-endian `uint16` として decode されます。

```python
if seg.segmentation_type is dicom.seg.SegmentationType.labelmap:
    labels = seg.to_array(0)
    print(labels.shape, labels.dtype)
    print(seg.present_segment_numbers(0))
```

出力例:

```text
(512, 512) uint16
(1, 24, 300)
```

Palette lookup、color mapping、opacity、legend rendering は viewer/UI layer の責任です。DicomSDL は保存された label sample と metadata を返し、palette image は描画しません。

特定の segment の `uint8` 0/1 mask が必要な場合は `mask_for_segment()` を使います。この API は BINARY、FRACTIONAL、LABELMAP SEG で共通に動作します。FRACTIONAL SEG では threshold が正規化された `[0, 1]` 単位で適用されます。

```python
segment_number = 24
mask = seg.mask_for_segment(0, segment_number)
print(mask.shape, mask.dtype, mask.min(), mask.max())
```

出力例:

```text
(512, 512) uint8 0 1
```

`present_segment_numbers(frame)` は指定された LABELMAP frame だけをスキャンして結果をキャッシュします。`frames_for_segment(segment_number)` は初回呼び出し時に全 LABELMAP frame をスキャンすることがあります。`validate_label_values()` は全 LABELMAP label を検証し、FRACTIONAL frame を decode して `MaximumFractionalValue` の範囲を確認するため、大きな multi-frame SEG では明示的な検証操作として扱ってください。
