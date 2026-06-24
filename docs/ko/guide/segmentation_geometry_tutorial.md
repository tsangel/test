# SEG와 Geometry

SEG metadata를 확인하고, SEG frame을 decode하고, image-plane geometry를 만들고,
slice stack을 계획하거나 mask와 image를 안전하게 overlay할 수 있는지 확인할 때
이 API를 사용한다.

아래 예제는 Python을 기준으로 하며, 일반적인 `dicom.read_file()` 흐름에서
이어진다. `dicom.geometry`는 viewer를 만들거나 최종 output volume을 할당하지
않는다. dominant grid 선택과 mask resampling도 caller가 결정한다.

mask 예제에는 NumPy가 필요하다.

```bash
pip install "dicomsdl[numpy]"
```

출력값은 입력 파일에 따라 달라진다. 아래 SEG 출력은 하나의 binary FDG/FBB brain
SEG sample에서 확인한 값을 일부만 발췌한 것이다.

## DICOM SEG 파일 열기

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
print(seg.maximum_fractional_value)
print(seg.frame_of_reference_uid)
print(seg.rows, seg.columns, seg.segment_count, seg.frame_count)
```

예시 출력:

```text
True
SegmentationType.binary
SegmentationFractionalType.none
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
Label Map Segmentation Storage 경로로 지원한다. SEG adapter는 open 시점에 모든
PixelData를 scan하지 않고 metadata만 index한다. LABELMAP stored label value는
frame decode/presence scan 시점이나 명시적인 `validate_label_values()` 호출 시점에
검증된다.

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

## BINARY SEG mask decode

BINARY SEG는 DICOM 안에 native 1-bit pixel로 저장된다. DicomSDL은 이를 unpack해서
값이 `0` 또는 `1`인 `uint8` mask로 돌려준다.

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

LABELMAP SEG는 label value를 PixelData에 직접 저장한다. Label value `0`은
background이고, 0이 아닌 값은 `SegmentSequence`의 segment number에 대응한다.
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
책임이다. DicomSDL은 stored label sample과 metadata를 반환하며 palette image를
렌더링하지 않는다.

특정 segment에 대한 semantic mask가 필요하면 `mask_for_segment()`를 사용한다. 이
API는 BINARY, FRACTIONAL, LABELMAP SEG에서 공통으로 동작한다. FRACTIONAL SEG에서는
threshold가 normalized `[0, 1]` 단위로 적용된다.

```python
segment_number = 24
mask = seg.mask_for_segment(0, segment_number)
print(mask.shape, mask.dtype, mask.min(), mask.max())
```

예시 출력:

```text
(512, 512) uint8 0 1
```

`present_segment_numbers(frame)`는 요청한 LABELMAP frame만 scan하고 결과를 cache한다.
`frames_for_segment(segment_number)`와 `validate_label_values()`는 처음 호출할 때
모든 LABELMAP frame을 scan할 수 있으므로, 큰 multi-frame SEG에서는 명시적인
validation/indexing 작업으로 취급하는 편이 좋다.

## DICOM plane tag를 geometry로 바꾸기

Geometry module은 DICOM image plane metadata를 검증된 객체로 바꿔준다.

```python
g = dicom.geometry

image = dicom.read_file(r"C:\data\CT-slice-0035.dcm")
plane = g.plane_from_single_frame_image(image)

world = plane.world_from_index(g.ImagePoint2D(100.0, 120.0))
index = plane.index_from_world(world)

print(world)
print(index)
```

1 mm axial slice라면 예시 출력은 다음처럼 보인다.

```text
Point3d(x=-28.000061, y=-11.25, z=-38.999939)
ImagePoint2D(i=100, j=120)
```

DicomSDL은 image index 이름을 이렇게 쓴다.

- `i`: column 방향 index.
- `j`: row 방향 index.
- `k`: volume의 slice/frame index.

DICOM `PixelSpacing`은 `[row_spacing, column_spacing]` 순서다. DicomSDL에서는
이를 `spacing_j`, `spacing_i`로 나누어 표현한다.

```python
print("columns, rows:", plane.columns, plane.rows)
print("spacing_i, spacing_j:", plane.spacing_i, plane.spacing_j)
```

예시 출력:

```text
columns, rows: 512 512
spacing_i, spacing_j: 1.0 1.0
```

## Classic slice stack plan

Classic CT/MR/PET series는 보통 slice마다 SOP instance가 하나씩 있다.
`plan_slice_stack()`은 여러 파일을 실제 물리 slice 순서로 정렬하고, volume
geometry와 placement list를 반환한다.

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

예시 출력:

```text
256 256 91
1.0 1.0 1.0
source file 0 frame 0 -> k 0
source file 1 frame 0 -> k 1
source file 2 frame 0 -> k 2
source file 3 frame 0 -> k 3
source file 4 frame 0 -> k 4
```

직접 pixel volume을 조립하려면 placement를 이용한다.

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

예시 출력:

```text
(91, 256, 256) uint16
```

`dicom.geometry`는 pixel을 decode하거나 output volume을 할당하지 않는다. 이 경계를
유지하면 viewer, batch job, resampling pipeline이 각자 필요한 memory layout과
정책을 선택할 수 있다.

## SEG/Image overlay compatibility

Overlay check는 이미 만들어진 geometry 객체와 frame of reference UID만 본다.
Dataset을 다시 순회하지 않으므로 가볍게 반복 호출할 수 있다.

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

같은 grid에 놓인 SEG frame이라면 예시 출력은 다음처럼 보인다.

```text
OverlayCompatibility.compatible
can_transform: True
can_direct_overlay: True
requires_resampling: False
overlaps_extent: True
target_k_range: 35 36
```

주요 필드는 다음처럼 해석한다.

- `can_direct_overlay`: grid가 충분히 같아서 index copy가 가능하다.
- `requires_resampling`: 같은 frame of reference 안에 있지만 grid mapping 때문에
  interpolation 또는 resampling이 필요하다.
- `different_extent`: grid는 같지만 한쪽 extent가 더 작거나 크다. 보통 crop, pad,
  clip 정책의 문제다.
- `different_frame_of_reference`: 외부 registration 없이 overlay하면 안 된다.

`can_transform`이 true라면 transform을 한 번 만들어 paint loop나 sampling loop에서
재사용한다.

```python
if check.can_transform:
    transform = g.make_plane_to_volume_transform(seg_plane, target_volume)
    center = g.ImagePoint2D(seg.columns / 2.0, seg.rows / 2.0)
    print(transform.target_index_from_source_index(center))
```

예시 출력:

```text
ImagePoint3D(i=128, j=128, k=35)
```

## Enhanced multi-frame image

Enhanced CT/MR/PET은 한 파일 안에 많은 frame을 담을 수 있다. 한 파일 안에 여러
stack, time point, phase, echo가 섞일 수도 있으므로 먼저 frame을 group으로 나눈다.

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

단일 enhanced CT stack이라면 예시 출력은 다음처럼 보인다.

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

`plane_from_multiframe_image(file, frame_index)`는 direct overlay를 위한 helper다.
일반 slice plane만 반환한다. `frame_geometry_from_multiframe_image()`는
`ImageFrameGeometryKind`를 보존하므로 sampled projection이나 distorted frame
metadata를 확인하면서도 이를 일반 slice처럼 잘못 취급하는 일을 피할 수 있다.

파일 전체가 단일 stack이라고 기대할 때만 `plan_image_frame_stack(file)`을 바로
사용한다. 여러 group이 있을 수 있다면 group별 frame list를 명시해서 plan을 만든다.

```python
single_plan = g.plan_image_frame_stack(file)
if not single_plan.ok:
    for issue in single_plan.issues:
        print(issue.status, issue.frame_index, issue.tag, issue.message)
```

파일 안에 stack이 둘 이상 있으면 다음처럼 보일 수 있다.

```text
SliceStackStatus.multiple_frame_stacks 0 (0020,9157) file contains multiple frame stacks
```

## Reconstructed NM TOMO stack

Nuclear Medicine Image Storage는 enhanced image와 다른 오래된 frame organization
tag를 사용한다. DicomSDL에는 reconstructed TOMO stack을 위한 전용 adapter가 있다.

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

예시 출력:

```text
128 128 64
[(0, 0), (1, 1), (2, 2), (3, 3), (4, 4), (5, 5), (6, 6), (7, 7), (8, 8), (9, 9)]
```

현재 NM adapter는 `RECON TOMO`와 `RECON GATED TOMO` 중 `NumberOfFrames`가 있고
`FrameIncrementPointer`가 정확히 하나의 값, `SliceVector`만 갖는 경우를 받는다.
Projection acquisition이나 time/energy 같은 추가 vector가 섞인 경우는 임의로
추측하지 않고 거부한다.

## Stack 실패 진단

Analysis 객체는 structured issue를 보관하므로 viewer나 batch script가 다음 동작을
정할 수 있다.

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

Non-uniform stack이라면 예시 출력은 다음처럼 보인다.

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

자주 보는 status는 다음과 같다.

- `missing_geometry`: 필요한 plane tag를 찾지 못했다.
- `missing_dimension_module`: enhanced grouping metadata가 없다.
- `multiple_frame_stacks`: 파일 전체 convenience call에서 stack이 둘 이상 발견됐다.
- `duplicate_slice_position`: 두 frame이 같은 slice 위치에 있다.
- `non_uniform_spacing`: stack은 분석할 수 있지만 하나의 uniform volume grid는 아니다.
- `inconsistent_slice_origin`: slice origin이 in-plane으로 흔들려 하나의 rectilinear
  affine volume으로 표현할 수 없다.

Non-uniform input에서도 `uniform_runs`는 유용한 연속 sub-range를 알려준다.
DicomSDL은 이를 보고하지만 dominant grid를 고르거나 그 grid로 resampling하지는
않는다.

## 테스트용 synthetic geometry 만들기

Overlay math를 테스트하는 데 실제 DICOM 파일이 꼭 필요하지는 않다. Geometry 객체를
직접 만들 수 있다.

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

예상 출력:

```text
OverlayCompatibility.compatible 4 5
ImagePoint3D(i=10, j=20, k=4)
```

이 방식은 실제 patient data를 테스트 suite에 넣지 않고도 DicomSDL geometry를
사용하는 application 코드를 unit test하기에 좋다.
