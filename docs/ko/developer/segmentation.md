# DICOM Segmentation MVP

이 문서는 DicomSDL의 첫 high-level DICOM SEG adapter 계약을 정리한다. core DICOM 읽기는 `dicom.h`에 두고, SEG 해석은 선택적으로 include하는 public header `dicom_seg.h`에 둔다.

## 지원 범위

- SOP Class는 BINARY/FRACTIONAL용 Segmentation Storage (`1.2.840.10008.5.1.4.1.1.66.4`)와 LABELMAP용 Label Map Segmentation Storage (`1.2.840.10008.5.1.4.1.1.66.7`)를 지원한다.
- BINARY SEG는 native 1-bit multi-frame PixelData를 지원한다. `decode_frame_into()`는 저장된 한 frame을 unpack해서 pixel당 1바이트, 값 `0` 또는 `1`로 돌려준다.
- FRACTIONAL SEG는 native 8-bit PixelData를 지원한다. decode 결과는 raw `uint8` sample이며, caller가 `raw_value / MaximumFractionalValue`로 fractional value를 해석한다.
- LABELMAP SEG는 Label Map Segmentation Storage에서 native uncompressed 8-bit 또는 16-bit stored label sample을 지원한다. decode는 저장된 label value를 보존하고, palette lookup/color rendering은 viewer/UI layer 책임으로 둔다.
- metadata view는 `SegmentSequence`, `PerFrameFunctionalGroupsSequence`, `SharedFunctionalGroupsSequence`, source image reference, `FrameOfReferenceUID`를 frame 단위로 index한다.

## Post-MVP

- frame mask들을 3D array로 조립하는 volume reconstruction API.
- SEG frame을 표시 대상 영상에 올리기 위한 affine/overlay helper.
- RLE 같은 encapsulated transfer syntax를 포함한 compressed/encapsulated SEG PixelData.

## 필수 Metadata

SEG adapter는 기본적으로 MVP에 필요한 metadata를 검증한다.

- `FrameOfReferenceUID`는 필수이며, SEG를 다른 영상에 직접 overlay할 수 있는지 판단하는 1차 key다. `SourceImageSequence`는 provenance metadata이므로 반드시 그 영상에만 표시해야 한다는 뜻은 아니다.
- `Rows`, `Columns`, `SegmentSequence`, `PerFrameFunctionalGroupsSequence`는 필수다.
- `SharedFunctionalGroupsSequence`는 정확히 하나의 item을 가져야 한다.
- BINARY/FRACTIONAL frame은 하나의 `ReferencedSegmentNumber`로 해석되어야 한다.
- FRACTIONAL SEG는 `SegmentationFractionalType`과 `MaximumFractionalValue`가 필요하다.
- LABELMAP SEG는 Label Map Segmentation Storage와 `SegmentationType=LABELMAP`, `BitsAllocated` 8 또는 16, unsigned single-sample pixel, `PhotometricInterpretation` `MONOCHROME2` 또는 `PALETTE COLOR`가 필요하다. Stored label value 검증은 file open 시점이 아니라 decode/presence query 또는 `validate_label_values()` 호출 시점에 lazy하게 수행한다.

조건을 만족하지 않으면 이상한 mask를 조용히 반환하지 말고 명확한 error를 내야 한다.

## Pixel 계약

BINARY SEG MVP는 native 1-bit DICOM PixelData를 지원한다. public API는 packed bit가 아니라 decoded 8-bit frame을 반환한다.

```cpp
std::vector<std::uint8_t> mask(seg->rows() * seg->columns());
seg->decode_frame_into(frame_index, mask);
// mask 값은 0 또는 1
```

FRACTIONAL SEG MVP는 저장된 raw 8-bit sample을 그대로 반환한다.

```python
raw = seg.to_array(0)  # dtype uint8
fraction = raw.astype("float32") / seg.maximum_fractional_value
```

scaling은 caller가 수행한다. probability/occupancy 소비자가 원하는 precision과 memory layout을 선택할 수 있게 하기 위해서다.

LABELMAP SEG에서 `to_array()`는 저장 sample dtype을 보존한다. 8-bit label map은 `uint8`, 16-bit label map은 native-endian `uint16`을 반환한다. `present_segment_numbers(frame)`는 해당 frame에 실제 등장한 non-background label을 반환하고, `mask_for_segment(frame, segment_number)`는 요청한 segment에 대한 semantic `uint8` 0/1 mask를 반환한다. unknown stored label value는 file open 시점에는 검사하지 않고, 해당 frame을 decode/scan하거나 `validate_label_values()`를 호출할 때 error로 보고한다.

## API Pattern

C++에서는 보통 SEG convenience reader를 사용한다. 이미 읽은 `DicomFile`을 재사용해야 하는 고급 경로에서는 `from_dicomfile()`로 소유권을 SEG adapter에 넘긴다.

```cpp
#include <dicom.h>
#include <dicom_seg.h>

auto seg = dicom::seg::read_file(path);

auto file = dicom::read_file(path);
auto seg_from_file = dicom::seg::from_dicomfile(std::move(file));
```

C++ adapter가 `DicomFile`을 소유하므로 segment/frame view는 내부 DICOM dataset을 복사하지 않고 빌려 쓴다. 문자열과 item copy를 피하면서 lifetime을 단순하게 유지하는 구조다.

Python에서도 같은 naming을 사용한다.

```python
import dicomsdl as dicom

seg = dicom.seg.read_file(path)
seg = dicom.seg.read_bytes(data, copy=False)
```

Python `dicom.seg.from_dicomfile(df)` helper는 제공하지 않는다. Python에서는 기존 `DicomFile` 객체에서 C++ unique ownership을 move할 수 없고, 이를 흉내 내려면 전체 dataset을 복사/재파싱해야 한다. 큰 SEG에서 너무 쉽게 비용이 커질 수 있으므로 Python API는 `read_file()`과 `read_bytes()`만 둔다.

## Regression Test

저장소에는 synthetic BINARY/FRACTIONAL/LABELMAP SEG C++/Python 테스트를 둔다. 실제 sample은 private data일 수 있으므로 환경변수로 optional local regression을 켠다.

```powershell
$env:DICOMSDL_SEG_SAMPLE_PATH = "C:\path\to\sample-seg.dcm"
python -m pytest tests/python/test_segmentation.py -q
```

Python wheel은 `package_data`로 stub을 포함한다. CMake target은 저장소의 `include/`를 노출하므로 source tree를 소비하는 build에서는 `<dicom_seg.h>`를 사용할 수 있다. 정식 CMake install/export rule은 이 MVP 밖의 작업이다.
