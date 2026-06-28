# DICOM Segmentation (SEG)

このページは、DicomSDL の high-level DICOM Segmentation adapter の公開契約をまとめたものです。DICOM では Segmentation は SEG modality（`Modality = SEG`）です。DicomSDL は core dataset の読み取りを `dicom.h` に置き、SEG の解釈は必要に応じて include する public header `dicom_seg.h` で提供します。

## 対応範囲

- SOP Class: BINARY/FRACTIONAL は Segmentation Storage（`1.2.840.10008.5.1.4.1.1.66.4`）、LABELMAP は Label Map Segmentation Storage（`1.2.840.10008.5.1.4.1.1.66.7`）をサポートします。
- BINARY SEG: native 1-bit multi-frame PixelData と Encapsulated Uncompressed frame payload の read/decode をサポートします。compressed BINARY SEG への pixel transcode、または compressed BINARY SEG からの transcode は、core pixel layer が stored `BitsAllocated=1` layout を end to end で表現できるようになるまで意図的に未対応です。
- FRACTIONAL SEG: 8-bit samples を native uncompressed、Encapsulated Uncompressed、対応 codec がある lossless compressed transfer syntax でサポートします。decode 結果は raw `uint8` samples で、caller は `raw_value / MaximumFractionalValue` で変換できます。
- LABELMAP SEG: Label Map Segmentation Storage で 8-bit と 16-bit の stored label samples を native uncompressed、Encapsulated Uncompressed、対応 codec がある lossless compressed transfer syntax でサポートします。decode は stored label value を保持します。palette lookup と color rendering は viewer/UI layer の責務です。
- lossy または near-lossless compressed SEG source/target は拒否します。Big Endian Label Map SEG はこの契約ではサポートしません。
- metadata view は `SegmentSequence`、`PerFrameFunctionalGroupsSequence`、`SharedFunctionalGroupsSequence`、source image reference、`FrameOfReferenceUID` を frame 単位で index します。

## Transfer Syntax 対応

| PixelData storage | BINARY | FRACTIONAL | LABELMAP |
| --- | --- | --- | --- |
| Native uncompressed Little Endian | read/decode 対応 | read/write/transcode 対応 | read/write/transcode 対応 |
| Native Explicit VR Big Endian | BINARY native read は generic DICOM path に従う | generic pixel path が対応する範囲のみ対応 | 未対応 |
| Encapsulated Uncompressed Explicit VR Little Endian | read/decode 対応。BINARY pixel transcode は未対応 | read/write/transcode 対応 | read/write/transcode 対応 |
| RLE Lossless、JPEG-LS Lossless、JPEG 2000 Lossless、HTJ2K Lossless、JPEG XL Lossless など、登録済み codec を持つ lossless compressed image syntax | core 1-bit layout/write support までは未対応 | read/write/transcode 対応 | read/write/transcode 対応 |
| Lossy または near-lossless compressed syntax | 拒否 | 拒否 | 拒否 |
| 未対応の compressed/video/referenced source codec | frame decode または transcode 時に拒否 | frame decode または transcode 時に拒否 | frame decode または transcode 時に拒否 |

## 必須 Metadata

SEG adapter は、安全に frame を解釈するために必要な metadata を既定で検証します。

- `FrameOfReferenceUID` は必須で、SEG を別の image に直接 overlay できるか判断する primary key です。`SourceImageSequence` は provenance metadata であり、必ずその image だけに表示するという意味ではありません。
- `Rows`、`Columns`、`SegmentSequence`、`PerFrameFunctionalGroupsSequence` は必須です。
- `SharedFunctionalGroupsSequence` は item をちょうど 1 つ持つ必要があります。
- BINARY/FRACTIONAL frame は 1 つの `ReferencedSegmentNumber` に解決される必要があります。
- FRACTIONAL SEG には `SegmentationFractionalType` と `MaximumFractionalValue` が必要です。Sample 値は decode/mask 生成時、または `validate_label_values()` 呼び出し時に `MaximumFractionalValue` 以下か検証されます。
- LABELMAP SEG には Label Map Segmentation Storage、`SegmentationType=LABELMAP`、`BitsAllocated` 8 または 16、unsigned single-sample pixels、`PhotometricInterpretation` `MONOCHROME2` または `PALETTE COLOR` が必要です。Stored label value の検証は file open 時には行わず、decode/presence query、または `validate_label_values()` 呼び出し時に lazy に行います。

これらの条件を満たさない場合、adapter は誤解を招く mask を静かに返さず、明確に失敗します。

## Pixel 契約

`to_array()` と `decode_frame()` は stored representation を保持します。BINARY、FRACTIONAL、LABELMAP を共通に扱い、特定 segment の semantic 0/1 mask が必要な場合は `mask_for_segment()` を使ってください。

BINARY SEG では、`decode_frame_into()` は stored 1-bit frame を unpack し、`uint8` の `0` または `1` として返します。

```cpp
std::vector<std::uint8_t> mask(seg->rows() * seg->columns());
seg->decode_frame_into(frame_index, mask);
// mask values are 0 or 1
```

FRACTIONAL SEG では、`to_array()` は raw `uint8` stored samples を返します。

```python
raw = seg.to_array(0)  # dtype uint8
fraction = raw.astype("float32") / seg.maximum_fractional_value
```

`mask_for_segment(..., fractional_threshold=...)` は semantic binary mask を作ります。既定の threshold `0.0` は `sample > 0` を意味し、それ以外の threshold では `sample / MaximumFractionalValue >= fractional_threshold` で比較します。

LABELMAP SEG では、`to_array()` は stored sample dtype を保持します。8-bit label map は `uint8`、16-bit label map は native-endian `uint16` です。`decode_frame()` は raw PixelData byte order ではなく、native typed sample bytes を返します。`present_segment_numbers(frame)` はその frame に実際に現れる non-background label を報告します。`PixelPaddingValue` がある場合、その segment number は background として扱われ、結果から除外されます。`mask_for_segment(frame, segment_number)` は要求された segment の semantic `uint8` 0/1 mask を返します。Unknown stored label value は file open 時には検査されず、該当 frame が decode/scan される時、または `validate_label_values()` が呼ばれる時に報告されます。

LABELMAP presence cache は lazy かつ thread-safe です。複数 thread が同じ frame の最初の presence query を同時に呼ぶと frame-local scan が重複することがありますが、ready cache entry と all-frame index は immutable で置き換えられません。All-frame index の構築は serialized されます。

`referenced_segment_number` は BINARY/FRACTIONAL の compatibility accessor として残ります。LABELMAP frame は複数の segment labels を含み得るため、LABELMAP でこの accessor を呼ぶと error になります。共通コードでは `present_segment_numbers()` と `mask_for_segment()` を使ってください。

`_into()` API は、decode または validation が例外を投げた場合、output buffer を partial write 状態にすることがあります。

## API Pattern

C++ では通常、SEG convenience reader を使います。すでに parsed `DicomFile` を所有している advanced caller は、`from_dicomfile()` で ownership を SEG adapter に移せます。

```cpp
#include <dicom.h>
#include <dicom_seg.h>

auto seg = dicom::seg::read_file(path);

auto file = dicom::read_file(path);
auto seg_from_file = dicom::seg::from_dicomfile(std::move(file));
```

C++ adapter は `DicomFile` を所有し、返される segment/frame view はその dataset を borrow します。これにより string や DICOM item の copy を避けながら、view lifetime を単純に保てます。

Python でも同じ naming を使います。

```python
import dicomsdl as dicom

seg = dicom.seg.read_file(path)
seg = dicom.seg.read_bytes(data, copy=False)
```

Python には `dicom.seg.from_dicomfile(df)` helper はありません。Python では既存の `DicomFile` object から C++ unique ownership を move できず、これを模倣するには大きな SEG dataset 全体を copy/reparse する必要があるため、誤って高コストな経路を選びやすくなります。

## Regression Tests

repository には synthetic BINARY、FRACTIONAL、LABELMAP SEG の C++/Python tests があります。private data を commit せずに local real-sample regression を有効にできます。

```powershell
$env:DICOMSDL_SEG_SAMPLE_PATH = "C:\path\to\sample-seg.dcm"
python -m pytest tests/python/test_segmentation.py -q
```

Python wheel は `package_data` で stub を含みます。CMake target は repository の `include/` directory を公開するため、source tree を消費する build では `<dicom_seg.h>` を使用できます。正式な CMake install/export rules はまだこの契約の範囲外です。
