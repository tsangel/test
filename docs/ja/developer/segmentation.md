# DICOM Segmentation MVP

このページは、high-level DICOM SEG adapter に対する DicomSDL の最初の契約をまとめたものです。core DICOM の読み取りは `dicom.h` に残し、SEG の解釈は任意で include する public header `dicom_seg.h` から公開します。

## 対応範囲

- SOP Class は、BINARY/FRACTIONAL 用の Segmentation Storage (`1.2.840.10008.5.1.4.1.1.66.4`) と、LABELMAP 用の Label Map Segmentation Storage (`1.2.840.10008.5.1.4.1.1.66.7`) に対応します。
- BINARY SEG は native 1-bit multi-frame PixelData に対応します。`decode_frame_into()` は保存された 1 frame を unpack し、pixel ごとに 1 byte、値 `0` または `1` として返します。
- FRACTIONAL SEG は native 8-bit PixelData に対応します。decode 結果は raw `uint8` sample で、呼び出し側が `raw_value / MaximumFractionalValue` で fractional value に変換します。
- LABELMAP SEG は Label Map Segmentation Storage で、native uncompressed 8-bit または 16-bit の stored label sample に対応します。decode は保存された label value を保持し、palette lookup や color rendering は viewer/UI layer の責任です。
- metadata view は `SegmentSequence`、`PerFrameFunctionalGroupsSequence`、`SharedFunctionalGroupsSequence`、source image reference、`FrameOfReferenceUID` を frame 単位で index します。

## Post-MVP

- frame mask を 3D array に組み立てる volume reconstruction API。
- SEG frame を表示対象 image に対応付ける affine / overlay helper。
- RLE などの encapsulated transfer syntax を含む compressed / encapsulated SEG PixelData。

## 必須 Metadata

SEG adapter は、デフォルトで MVP に必要な metadata を検証します。

- `FrameOfReferenceUID` は必須で、SEG を別の image に直接 overlay できるかを判断する primary key です。`SourceImageSequence` は provenance metadata であり、必ずその image だけを表示対象にしなければならないという意味ではありません。
- `Rows`、`Columns`、`SegmentSequence`、`PerFrameFunctionalGroupsSequence` は必須です。
- `SharedFunctionalGroupsSequence` は item を 1 つだけ持つ必要があります。
- BINARY/FRACTIONAL frame は 1 つの `ReferencedSegmentNumber` に解決できる必要があります。
- FRACTIONAL SEG には `SegmentationFractionalType` と `MaximumFractionalValue` が必要です。
- LABELMAP SEG には Label Map Segmentation Storage、`SegmentationType=LABELMAP`、`BitsAllocated` 8 または 16、unsigned single-sample pixel、`PhotometricInterpretation` `MONOCHROME2` または `PALETTE COLOR` が必要です。stored label value の検証は file open 時点ではなく、decode / presence query または `validate_label_values()` 呼び出し時に lazy に実行されます。

条件を満たさない場合、adapter は誤解を招く mask を返さず、明確な error にします。

## Pixel 契約

BINARY SEG MVP は native 1-bit DICOM PixelData に対応します。public API は packed bit ではなく、decoded 8-bit frame を返します。

```cpp
std::vector<std::uint8_t> mask(seg->rows() * seg->columns());
seg->decode_frame_into(frame_index, mask);
// mask values are 0 or 1
```

FRACTIONAL SEG MVP は保存された raw 8-bit sample を返します。

```python
raw = seg.to_array(0)  # dtype uint8
fraction = raw.astype("float32") / seg.maximum_fractional_value
```

scaling は呼び出し側で行います。probability / occupancy の利用側が、必要な precision と memory layout を選べるようにするためです。

LABELMAP SEG では、`to_array()` は保存 sample の dtype を保持します。8-bit label map は `uint8`、16-bit label map は native-endian `uint16` を返します。`present_segment_numbers(frame)` はその frame に実際に存在する non-background label を返し、`mask_for_segment(frame, segment_number)` は要求された segment の semantic `uint8` 0/1 mask を返します。unknown stored label value は file open 時点では検査せず、該当 frame を decode / scan する時、または `validate_label_values()` を呼び出す時に error として報告されます。

## API Pattern

C++ では通常、SEG convenience reader を使います。すでに parse 済みの `DicomFile` を再利用する advanced path では、`from_dicomfile()` で所有権を SEG adapter に移します。

```cpp
#include <dicom.h>
#include <dicom_seg.h>

auto seg = dicom::seg::read_file(path);

auto file = dicom::read_file(path);
auto seg_from_file = dicom::seg::from_dicomfile(std::move(file));
```

C++ adapter が `DicomFile` を所有するため、segment / frame view は内部 DICOM dataset を copy せずに borrow します。string や item の copy を避けつつ、lifetime を単純に保つ構造です。

Python でも同じ naming を使います。

```python
import dicomsdl as dicom

seg = dicom.seg.read_file(path)
seg = dicom.seg.read_bytes(data, copy=False)
```

Python `dicom.seg.from_dicomfile(df)` helper は提供しません。Python では既存の `DicomFile` object から C++ unique ownership を move できず、それをまねるには dataset 全体の copy / reparse が必要になります。大きな SEG でコストが簡単に膨らむため、Python API は `read_file()` と `read_bytes()` のみを入口にします。

## Regression Tests

repository には synthetic BINARY/FRACTIONAL/LABELMAP SEG の C++ / Python test を置きます。実 sample は private data である場合があるため、環境変数で optional local regression を有効化します。

```powershell
$env:DICOMSDL_SEG_SAMPLE_PATH = "C:\path\to\sample-seg.dcm"
python -m pytest tests/python/test_segmentation.py -q
```

Python wheel は `package_data` として stub を含みます。CMake target は repository の `include/` を公開するため、source tree を使う build では `<dicom_seg.h>` を利用できます。正式な CMake install/export rule はこの MVP の範囲外です。
