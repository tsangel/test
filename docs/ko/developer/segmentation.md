# DICOM Segmentation (SEG)

이 문서는 DicomSDL high-level DICOM Segmentation adapter의 공개 계약을 정리합니다. DICOM에서 Segmentation은 SEG modality(`Modality = SEG`)입니다. DicomSDL은 core dataset 읽기를 `dicom.h`에 두고, SEG 해석은 선택적으로 include하는 public header `dicom_seg.h`를 통해 제공합니다.

## 지원 범위

- SOP Class: BINARY/FRACTIONAL은 Segmentation Storage(`1.2.840.10008.5.1.4.1.1.66.4`), LABELMAP은 Label Map Segmentation Storage(`1.2.840.10008.5.1.4.1.1.66.7`)를 지원합니다.
- BINARY SEG: native 1-bit multi-frame PixelData의 read/decode를 지원합니다. compressed BINARY SEG로의 pixel transcode 또는 compressed BINARY SEG에서의 transcode는 core pixel layer가 stored `BitsAllocated=1` layout을 끝까지 표현할 수 있게 된 뒤 지원합니다.
- FRACTIONAL SEG: 8-bit sample을 native uncompressed, Encapsulated Uncompressed, 그리고 codec이 있는 lossless compressed transfer syntax에서 지원합니다. decode 결과는 raw `uint8` sample이며, caller가 `raw_value / MaximumFractionalValue`로 변환할 수 있습니다.
- LABELMAP SEG: Label Map Segmentation Storage에서 8-bit와 16-bit stored label sample을 native uncompressed, Encapsulated Uncompressed, 그리고 codec이 있는 lossless compressed transfer syntax로 지원합니다. decode는 stored label value를 보존하며, palette lookup과 color rendering은 viewer/UI layer의 책임입니다.
- lossy 또는 near-lossless compressed SEG source/target은 거부합니다. Big Endian Label Map SEG는 이 계약에서 지원하지 않습니다.
- metadata view는 `SegmentSequence`, `PerFrameFunctionalGroupsSequence`, `SharedFunctionalGroupsSequence`, source image reference, `FrameOfReferenceUID`를 frame 단위로 index합니다.

## Transfer Syntax 지원

| PixelData storage | BINARY | FRACTIONAL | LABELMAP |
| --- | --- | --- | --- |
| Native uncompressed Little Endian | read/decode 지원 | read/write/transcode 지원 | read/write/transcode 지원 |
| Native Explicit VR Big Endian | BINARY native read는 generic DICOM path를 따름 | generic pixel path가 지원하는 범위에서만 지원 | 미지원 |
| Encapsulated Uncompressed Explicit VR Little Endian | BINARY pixel transcode 미지원 | read/write/transcode 지원 | read/write/transcode 지원 |
| RLE Lossless, JPEG-LS Lossless, JPEG 2000 Lossless, HTJ2K Lossless, JPEG XL Lossless 같은 codec 등록된 lossless compressed image syntax | core 1-bit layout/write 지원 전까지 미지원 | read/write/transcode 지원 | read/write/transcode 지원 |
| Lossy 또는 near-lossless compressed syntax | 거부 | 거부 | 거부 |
| 지원하지 않는 compressed/video/referenced source codec | frame decode 또는 transcode 시점에 거부 | frame decode 또는 transcode 시점에 거부 | frame decode 또는 transcode 시점에 거부 |

## 필수 Metadata

SEG adapter는 안전한 frame 해석에 필요한 metadata를 기본적으로 검증합니다.

- `FrameOfReferenceUID`는 필수이며, SEG를 다른 image 위에 직접 overlay할 수 있는지 판단하는 primary key입니다. `SourceImageSequence`는 provenance metadata이며, 반드시 그 image에만 표시해야 한다는 뜻은 아닙니다.
- `Rows`, `Columns`, `SegmentSequence`, `PerFrameFunctionalGroupsSequence`는 필수입니다.
- `SharedFunctionalGroupsSequence`는 정확히 하나의 item을 가져야 합니다.
- BINARY/FRACTIONAL frame은 하나의 `ReferencedSegmentNumber`로 해석되어야 합니다.
- FRACTIONAL SEG는 `SegmentationFractionalType`과 `MaximumFractionalValue`를 가져야 합니다. Sample 값은 decode/mask 생성 시점이나 `validate_label_values()` 호출 시점에 `MaximumFractionalValue` 이하인지 검증됩니다.
- LABELMAP SEG는 Label Map Segmentation Storage, `SegmentationType=LABELMAP`, `BitsAllocated` 8 또는 16, unsigned single-sample pixel, `PhotometricInterpretation` `MONOCHROME2` 또는 `PALETTE COLOR`를 요구합니다. Stored label value 검증은 file open 시점에 하지 않고, decode/presence query 시점이나 `validate_label_values()` 호출 시점에 lazy하게 수행합니다.

조건이 맞지 않으면 adapter는 오해의 소지가 있는 mask를 조용히 반환하지 않고 명확하게 실패합니다.

## Pixel 계약

`to_array()`와 `decode_frame()`은 저장 표현을 보존합니다. BINARY, FRACTIONAL, LABELMAP을 공통으로 다루면서 특정 segment의 semantic 0/1 mask가 필요하면 `mask_for_segment()`를 사용하세요.

BINARY SEG에서 `decode_frame_into()`는 stored 1-bit frame 하나를 unpack하여 `uint8` 값 `0` 또는 `1`로 반환합니다.

```cpp
std::vector<std::uint8_t> mask(seg->rows() * seg->columns());
seg->decode_frame_into(frame_index, mask);
// mask values are 0 or 1
```

FRACTIONAL SEG에서 `to_array()`는 raw `uint8` stored sample을 반환합니다.

```python
raw = seg.to_array(0)  # dtype uint8
fraction = raw.astype("float32") / seg.maximum_fractional_value
```

`mask_for_segment(..., fractional_threshold=...)`는 semantic binary mask를 만듭니다. 기본 threshold `0.0`은 `sample > 0`을 뜻하고, 그 외 threshold는 `sample / MaximumFractionalValue >= fractional_threshold`로 비교합니다.

LABELMAP SEG에서 `to_array()`는 stored sample dtype을 보존합니다. 8-bit label map은 `uint8`, 16-bit label map은 native-endian `uint16`입니다. `decode_frame()`은 raw PixelData byte order가 아니라 native typed sample bytes를 반환합니다. `present_segment_numbers(frame)`는 해당 frame에 실제로 등장한 non-background label을 보고합니다. `PixelPaddingValue`가 있으면 그 segment number를 background로 취급하여 제외합니다. `mask_for_segment(frame, segment_number)`는 요청한 segment의 semantic `uint8` 0/1 mask를 반환합니다. Unknown stored label value는 file open 시점에는 검사하지 않고, 관련 frame이 decode/scan되거나 `validate_label_values()`가 호출될 때 보고합니다.

LABELMAP presence cache는 lazy하고 thread-safe합니다. 여러 thread가 같은 frame의 첫 presence query를 동시에 호출하면 frame-local scan이 중복될 수 있지만, ready cache entry와 all-frame index는 immutable이며 교체되지 않습니다. All-frame index 생성은 serialized됩니다.

`referenced_segment_number`는 BINARY/FRACTIONAL을 위한 compatibility accessor로 유지됩니다. LABELMAP frame은 여러 segment label을 포함할 수 있으므로 LABELMAP에서 이 accessor를 호출하면 error가 납니다. 공통 코드는 `present_segment_numbers()`와 `mask_for_segment()`를 사용하세요.

`_into()` API는 decode 또는 validation 예외가 발생하면 output buffer를 partial write 상태로 남길 수 있습니다.

## API Pattern

C++에서는 보통 SEG convenience reader를 사용합니다. 이미 parse된 `DicomFile`을 가진 advanced caller는 `from_dicomfile()`로 ownership을 SEG adapter에 넘길 수 있습니다.

```cpp
#include <dicom.h>
#include <dicom_seg.h>

auto seg = dicom::seg::read_file(path);

auto file = dicom::read_file(path);
auto seg_from_file = dicom::seg::from_dicomfile(std::move(file));
```

C++ adapter는 `DicomFile`을 소유하고, 반환되는 segment/frame view는 그 dataset을 borrow합니다. 이렇게 하면 string과 DICOM item copy를 피하면서 view lifetime을 단순하게 유지할 수 있습니다.

Python에서도 같은 naming을 사용합니다.

```python
import dicomsdl as dicom

seg = dicom.seg.read_file(path)
seg = dicom.seg.read_bytes(data, copy=False)
```

Python에는 `dicom.seg.from_dicomfile(df)` helper가 없습니다. Python에서 기존 `DicomFile` 객체의 ownership을 C++ 쪽으로 move할 수 없고, 이를 흉내 내려면 큰 SEG dataset 전체를 copy/reparse해야 해서 실수로 비용이 커지기 쉽습니다.

## Regression Tests

저장소에는 synthetic BINARY, FRACTIONAL, LABELMAP SEG C++/Python test가 있습니다. private data를 commit하지 않고 local real-sample regression을 켤 수도 있습니다.

```powershell
$env:DICOMSDL_SEG_SAMPLE_PATH = "C:\path\to\sample-seg.dcm"
python -m pytest tests/python/test_segmentation.py -q
```

Python wheel은 `package_data`로 stub을 포함합니다. CMake target은 repository `include/` directory를 노출하므로 source tree를 사용하는 build에서 `<dicom_seg.h>`를 사용할 수 있습니다. 정식 CMake install/export rule은 아직 이 계약의 범위 밖입니다.
