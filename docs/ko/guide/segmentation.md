# DICOM Segmentation(SEG)

DICOM Segmentation(SEG) metadata를 확인하고, SEG frame을 decode하거나, BINARY/FRACTIONAL/LABELMAP SEG에서 semantic mask를 만들 때 이 가이드를 사용하세요.

DICOM 파일에서 이 객체들은 `Modality (0008,0060) = SEG`를 사용합니다. 정확한 저장 형식 식별자는 SOP Class입니다. BINARY/FRACTIONAL SEG는 Segmentation Storage를, LABELMAP SEG는 Label Map Segmentation Storage를 사용합니다.

mask 예제에는 NumPy support를 설치하세요:

```bash
pip install "dicomsdl[numpy]"
```

출력 값은 입력 파일에 따라 달라집니다. 아래 SEG excerpt는 binary FDG/FBB brain SEG sample에서 가져온 예시입니다.

## DICOM Segmentation(SEG) 파일 열기

DICOM Segmentation Storage와 Label Map Segmentation Storage는
`dicom.seg.read_file()`로 연다. 반환값은 일반 `DicomFile`이 아니라
`Segmentation` 객체다.

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

예시 출력:

```text
True
SegmentationType.binary
SegmentationFractionalType.none
SegmentsOverlap.undefined
None
1.3.6.1.4.1.43046.3.380371456.2303.1779756601.801016
256 256 97 2885
```

이미 bytes를 갖고 있다면 `read_bytes()`를 사용한다.

```python
data = seg_path.read_bytes()
seg = dicom.seg.read_bytes(data, copy=False)
```

Python SEG 입력 경로는 `read_file()`과 `read_bytes()`다. Python에는
`dicom.seg.from_dicomfile(df)`가 없다. 기존 `DicomFile`을 SEG 객체로 바꾸려면
dataset을 복사하고 다시 파싱해야 하기 때문이다.

DicomSDL은 BINARY/FRACTIONAL SEG를 Segmentation Storage 경로로, LABELMAP SEG를
Label Map Segmentation Storage 경로로 지원한다. SEG 어댑터는 파일을 열 때 전체
PixelData element를 스캔하지 않고 메타데이터만 인덱싱한다. LABELMAP에 저장된 label
value는 frame을 decode하거나 presence scan을 할 때, 또는 명시적인
`validate_label_values()` 호출 시점에 검증된다.

## Segment 목록 보기

DICOM SEG에서 `SegmentSequence`는 이 객체가 담고 있는 label을 설명한다. 각 item은
하나의 의미 단위이며, 뇌 구조, 종양, 장기, 알고리즘이 만든 mask class 등이 여기에
해당한다.

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

출력 일부:

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

DICOM segment number로 바로 찾을 수도 있다.

```python
left_white_matter = seg.segment_by_number(1)
if left_white_matter is not None:
    print(left_white_matter.label)
```

예시 출력:

```text
Left-Cerebral-White-Matter
```

Segment number는 Python list index와 다르다. frame과 label을 맞출 때는
`segment.number`를 기준으로 생각하는 편이 안전하다.

## SEG frame 보기

SEG Pixel Data는 multi-frame이다. BINARY/FRACTIONAL SEG에서는 저장된 frame 하나가
하나의 referenced segment number에 속한다. 보통 한 segment가 여러 frame을 갖고,
각 frame이 한 slice의 mask가 된다.

```python
frame = seg.frames[0]

print(frame.index)
print(frame.referenced_segment_number)
print(frame.image_position_patient)
print(frame.image_orientation_patient)
print(frame.pixel_spacing)
print(frame.slice_thickness)
```

예시 출력:

```text
0
1
(-128.000061, -131.25, -38.999939)
(1.0, 0.0, 0.0, 0.0, 1.0, 0.0)
(1.0, 1.0)
1.0
```

LABELMAP까지 같이 처리하는 코드를 만들 때는 `present_segment_numbers()`를 사용한다.
BINARY/FRACTIONAL SEG에서는 선언된 `ReferencedSegmentNumber`를 반환하고, LABELMAP
SEG에서는 해당 frame에 실제 등장한 non-background label value를 반환한다.
`referenced_segment_number`는 호환용 accessor이며 LABELMAP frame에서는 정의되지
않는다.

```python
print(frame.present_segment_numbers())
```

BINARY frame의 예시 출력:

```text
(1,)
```

Frame에는 source image reference가 있을 수 있다.

```python
for ref in frame.source_images:
    print(ref.sop_class_uid)
    print(ref.sop_instance_uid)
    print(ref.referenced_frame_numbers)
```

예시 출력:

```text
1.2.840.10008.5.1.4.1.1.2
1.2.840.113619.2.80.981715802.8664.151072595.1914331.90
[]
```

Source image reference는 SEG가 어떤 image를 바탕으로 만들어졌는지 알려주는 생성
이력 metadata다. 하지만 overlay target이 반드시 그 image여야 한다는 뜻은 아니다.
Overlay에서는 먼저 `FrameOfReferenceUID`를 비교한다.

## SegmentationType별 API 선택

`seg.segmentation_type`은 PixelData의 저장 표현을 알려준다. BINARY, FRACTIONAL,
LABELMAP SEG를 같이 다루는 코드는 다음 차이를 기준으로 잡는 것이 좋다.

| SegmentationType | `to_array()` / `decode_frame()` | segment membership | 권장 접근 |
| --- | --- | --- | --- |
| `binary` | `uint8` mask 값 `0` 또는 `1` | frame마다 하나의 `ReferencedSegmentNumber` | segment 단위 순회는 `frames_for_segment()`를 사용한다. `to_array()` 결과가 이미 그 frame의 segment mask다. |
| `fractional` | 저장된 raw `uint8` sample | frame마다 하나의 `ReferencedSegmentNumber` | threshold mask는 `mask_for_segment(..., fractional_threshold=...)`를 사용하고, raw probability/occupancy 값이 필요하면 `MaximumFractionalValue`로 직접 scaling한다. |
| `labelmap` | 저장된 label value, `uint8` 또는 native-endian `uint16` | 한 frame에 여러 segment number가 들어갈 수 있음 | `present_segment_numbers()`와 `mask_for_segment()`를 사용한다. LABELMAP frame에서는 `referenced_segment_number`를 쓰지 않는다. |

가장 안전한 공통 패턴은 다음과 같다.

```python
for frame in seg.frames:
    for segment_number in frame.present_segment_numbers():
        mask = frame.mask_for_segment(segment_number)
        # frame.image_position_patient / geometry mapping과 함께 mask를 사용한다.
```

저장된 pixel 표현이 필요하면 `to_array()`를 사용한다. 저장 방식과 무관한
`uint8` 0/1 segment mask가 필요하면 `mask_for_segment()`를 사용한다. LABELMAP에서
`frames_for_segment()`와 `validate_label_values()`는 처음 호출할 때 모든 frame을
스캔할 수 있다. BINARY/FRACTIONAL에서는 파일을 열 때 만든 메타데이터 index를 사용한다.

## BINARY SEG mask decode

BINARY SEG는 DICOM 안에 1-bit pixel로 저장된다. DicomSDL은 이를 unpack해서
값이 `0` 또는 `1`인 `uint8` mask로 돌려준다. Native uncompressed와 Encapsulated
Uncompressed BINARY SEG를 이 방식으로 읽을 수 있다. C++에서 frame 전체를
unpack하지 않고 set bit만 순회하려면 `binary_frame_bits()`와
`for_each_binary_frame_set_bit()`를 사용한다.

```python
mask = seg.to_array(0)
print(mask.shape, mask.dtype)
print(mask.min(), mask.max())
```

예시 출력:

```text
(256, 256) uint8
0 1
```

특정 segment에 속한 frame들을 모두 decode한다.

```python
masks_for_segment = []

for frame in seg.frames_for_segment(1):
    mask = frame.to_array()
    masks_for_segment.append((frame.index, mask))

print("decoded frames:", len(masks_for_segment))
```

예시 출력:

```text
decoded frames: 87
```

출력 배열을 재사용하고 싶다면 `decode_frame_into()`를 사용한다.

```python
import numpy as np

out = np.empty((seg.rows, seg.columns), dtype=np.uint8)
returned = seg.decode_frame_into(0, out)
print(returned is out)
print(out.shape, out.dtype, out.min(), out.max())
```

예시 출력:

```text
True
(256, 256) uint8 0 1
```

## BINARY SEG frame을 하나의 label volume으로 pack하기

이 기능이 가장 필요한 경우는 서로 거의 겹치지 않는 많은 수의 BINARY segment가 들어
있는 SEG다. Viewer 같은 application을 만들 때 이런 경우 segment마다 3D mask texture를
만들면 memory와 GPU binding 작업이 낭비된다. DicomSDL은 이런 BINARY SEG frame들을
하나의 `uint16` label volume으로 pack할 수 있다. Target grid 선택과 각 SEG frame을
어떤 slice에 둘지는 application이 결정한다.

```python
import numpy as np

# application의 image geometry / stack 코드가 만든 값이다.
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

예시 출력:

```text
(127, 256, 256) uint16
[0, 1, 2, 3]
```

`labels`의 shape은 `(slices, rows, columns)`다. Code `0`은 background다.
나머지 값은 runtime label code이며, 어떤 code는 두 개 이상의 segment overlap을
나타낼 수 있다. Semantic membership이 필요하면 반환된 `BinaryLabelVolume` 객체의
table API를 사용한다.

```python
segment_number = 2
label_id = packed.label_id_for_segment_number(segment_number)
mask = packed.restore_mask_for_segment(segment_number)

code = int(labels[30, 120, 90])
print(packed.label_set(code))
```

Display LUT가 필요하면 dense `label_id -> RGBA` table을 넘긴다. 반환 배열은
shape `(256, 256, 4)`인 RGBA8 LUT라서 저장된 label code로 바로 index할 수 있다.
Overlap label code의 색은 포함된 label들의 RGBA 평균으로 만든다. 다른 blend 정책이
필요한 application은 `label_set()`으로 label 구성을 읽어서 LUT를 직접 만들면 된다.

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
assert rgba_lut.shape == (256, 256, 4)
```

Viewer pipeline이 이미 staging buffer를 소유하고 있다면 allocation-free variant를
사용한다.

```python
label_volume = np.empty((slice_count, seg.rows, seg.columns), dtype=np.uint16)

packed = seg.build_binary_label_volume_into(
    label_volume,
    frame_placements,
    slices=slice_count,
)

assert np.shares_memory(packed.label_volume, label_volume)
```

`build_binary_label_volume_into()`는 application이 CPU/GPU staging allocation을 직접
관리하려는 경우에 권장된다. Visibility, color, opacity, lookup-table rendering은
viewer 책임으로 남는다.

## FRACTIONAL SEG mask decode

FRACTIONAL SEG는 8-bit raw sample을 저장한다. DicomSDL은 raw sample을 그대로
돌려주고, scaling은 caller가 명시적으로 수행한다.

```python
import numpy as np

if seg.segmentation_type is dicom.seg.SegmentationType.fractional:
    if not seg.maximum_fractional_value:
        raise ValueError("MaximumFractionalValue is missing")

    raw = seg.to_array(0)  # dtype uint8
    values = raw.astype(np.float32) / float(seg.maximum_fractional_value)
    print(values.min(), values.max())
```

위 예시 sample은 BINARY SEG라서 이 FRACTIONAL 블록은 아무 것도 출력하지 않는다.
Raw 값이 전체 저장 범위를 사용하는 FRACTIONAL SEG라면 출력은 보통 다음처럼 보인다.

```text
0.0 1.0
```

이렇게 두면 probability, occupancy, thresholded boolean mask 같은 downstream
처리에서 필요한 precision과 layout을 직접 선택할 수 있다.

## LABELMAP SEG frame decode

LABELMAP SEG는 label value를 PixelData에 직접 저장한다. 저장된 값은
`SegmentSequence`의 segment number에 대응한다. `PixelPaddingValue`가 있으면 그
segment number를 background로 취급하고 `present_segment_numbers()`에서는 제외한다.
DicomSDL은 저장 표현을 보존한다. 8-bit label map은 `uint8`, 16-bit label map은
native-endian `uint16`으로 decode된다.

```python
if seg.segmentation_type is dicom.seg.SegmentationType.labelmap:
    labels = seg.to_array(0)
    print(labels.shape, labels.dtype)
    print(seg.present_segment_numbers(0))
```

예시 출력:

```text
(512, 512) uint16
(1, 24, 300)
```

Palette lookup, color mapping, opacity, legend rendering은 viewer/UI layer의
책임이다. DicomSDL은 저장된 label sample과 metadata를 반환하며 palette image를
렌더링하지 않는다.

특정 segment에 대한 `uint8` 0/1 mask가 필요하면 `mask_for_segment()`를 사용한다. 이
API는 BINARY, FRACTIONAL, LABELMAP SEG에서 공통으로 동작한다. FRACTIONAL SEG에서는
threshold가 정규화된 `[0, 1]` 단위로 적용된다.

```python
segment_number = 24
mask = seg.mask_for_segment(0, segment_number)
print(mask.shape, mask.dtype, mask.min(), mask.max())
```

예시 출력:

```text
(512, 512) uint8 0 1
```

`present_segment_numbers(frame)`는 요청한 LABELMAP frame만 스캔하고 결과를 캐시한다.
`frames_for_segment(segment_number)`는 처음 호출할 때 모든 LABELMAP frame을
스캔할 수 있다. `validate_label_values()`는 모든 LABELMAP label을 검증하고
FRACTIONAL frame을 decode해 `MaximumFractionalValue` 범위를 확인하므로, 큰
multi-frame SEG에서는 명시적인 검증 작업으로 취급하는 편이 좋다.
