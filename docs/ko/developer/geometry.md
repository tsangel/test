# DICOM Geometry Helpers

이 문서는 `dicom_geometry.h`와 Python `dicom.geometry` 모듈의 첫 public
계약을 정리한다. geometry layer는 viewer, viewport, pixel buffer,
resampling 정책과 분리한다.

## 범위

- 검증된 2D image plane geometry와 rectilinear 3D volume geometry 생성
- classic single-frame image의 root DICOM tag 기반 geometry 해석
- enhanced multi-frame image의
  `PerFrameFunctionalGroupsSequence -> SharedFunctionalGroupsSequence -> root`
  순서 geometry 해석
- DICOM SEG frame geometry 해석. SEG는 root image-plane tag로 조용히
  fallback하지 않는다.
- 이미 만들어진 geometry 객체끼리 overlay compatibility 확인
- uniform slice stack을 volume geometry와 placement list로 planning

이 layer는 pixel decode, output volume allocation, non-uniform data의
dominant grid 선택, resampling을 수행하지 않는다.

## 좌표 계약

DicomSDL은 image index 이름으로 `i`, `j`, `k`를 사용한다.

- `i`: column 쪽 image index
- `j`: row 쪽 image index
- `k`: rectilinear volume의 slice/frame index

DICOM `ImageOrientationPatient`의 첫 triplet은 DicomSDL `direction_i`, 두
번째 triplet은 `direction_j`로 매핑한다. DICOM `PixelSpacing =
[row_spacing, column_spacing]`은 `spacing_j = row_spacing`,
`spacing_i = column_spacing`으로 매핑한다.

`ImagePlaneGeometry::index_to_world_matrix()`는 `(i, j, normal_mm, 1)`을
patient/world space에 embed한다. inverse는 `(i, j,
signed_normal_distance_mm, 1)`을 반환한다.

## C++ 사용

```cpp
#include <dicom.h>
#include <dicom_geometry.h>

auto file = dicom::read_file(path);
auto plane = dicom::geometry::plane_from_single_frame_image(*file);
if (!plane.ok()) {
    // plane.status(), plane.tag(), plane.message()를 확인한다.
}

auto world = plane.value().world_from_index({12.0, 8.0});
auto index = plane.value().index_from_world(world);
```

Enhanced multi-frame image는 frame 단위로 geometry를 읽을 수 있다.

```cpp
dicom::geometry::FrameGeometryReader reader(*file);
auto frame0 = reader.plane(0);
```

드문 접근은 convenience function을 쓰고, 많은 frame을 반복해서 읽는
코드는 `FrameGeometryReader`를 재사용한다. `plane()`은 regular slice
plane만 반환하며 `SAMPLED`/`DISTORTED` frame geometry는 거부한다. 그런
frame kind를 검사해야 하는 caller는 `image_frame_geometry()`를 사용한다.

## Python 사용

Python은 같은 개념을 `dicom.geometry`로 노출한다. Python에서는
`GeometryBuildResult<T>`를 직접 노출하지 않고 실패 시 `ValueError`를
던진다.

```python
import dicomsdl as dicom

g = dicom.geometry
plane = g.make_image_plane_geometry(
    g.Point3d(0.0, 0.0, 10.0),
    g.Vec3d(1.0, 0.0, 0.0),
    g.Vec3d(0.0, 1.0, 0.0),
    g.ImageSpacing2D(1.0, 1.0),
    g.ImageSize2D(512, 512),
)
world = plane.world_from_index(g.ImagePoint2D(20.0, 30.0))
```

## Overlay Check

Overlay check는 이미 만들어진 geometry 객체에 대한 O(1) preflight다.
DICOM dataset을 다시 순회하지 않고 transform buffer도 만들지 않는다.

```python
check = dicom.geometry.check_overlay_compatibility(
    source_for_uid, seg_plane, target_for_uid, image_volume
)
if check.can_direct_overlay:
    pass
elif check.can_transform and check.requires_resampling:
    pass
```

`can_transform`은 보통 두 객체가 사용할 수 있는 같은 frame of reference에
있다는 뜻이다. 다만 `OverlayCheckOptions.require_same_grid`가 true이면
직접 overlay 가능한 같은 grid일 때만 true가 된다. `can_direct_overlay`는
index copy가 가능할 만큼 grid가 맞는다는 뜻이다. `requires_resampling`은
실패가 아니라 물리 extent는 겹치지만 직접 overlay는 불가능해 caller가
정책을 정해야 하는 상태다.

## Slice Stack Planning

`analyze_slice_stack()`은 `SliceStackInput` 목록 또는 classic
`DataSet`/`DicomFile` 목록을 받는다. 결과에는 정렬된 slice, spacing gap,
uniform spacing, structured issue가 들어 있다.

`plan_slice_stack()`은 uniform rectilinear stack에서만 성공한다. 성공하면
`volume_geometry`는 output grid를 설명하고, `placements`는 어떤 입력 frame을
어느 target `k`에 넣어야 하는지 알려준다.

```python
inputs = [
    g.SliceStackInput(plane0, "1.2.3", source_index=0, frame_index=0),
    g.SliceStackInput(plane1, "1.2.3", source_index=1, frame_index=0),
]
plan = g.plan_slice_stack(inputs)
if plan.ok:
    for item in plan.placements:
        # sources[item.source_index]를 decode한 뒤 item.target_k에 넣는다.
        pass
```

Enhanced multi-frame object가 여러 stack, time point, phase, echo를 담을 수
있다면 먼저 `analyze_image_frame_stacks()`를 사용한다. 단일 stack convenience
function인 `analyze_image_frame_stack()`과 `plan_image_frame_stack()`은 파일이
하나의 stack으로 해석 가능하거나 명시적인 frame index 목록을 넘긴 경우에만
성공한다.

## 제한

- Non-uniform 또는 non-rectilinear stack은 `ImageVolumeGeometry`로 억지 변환하지
  않고 issue로 보고한다.
- `VolumetricProperties=SAMPLED`와 `DISTORTED`는
  `frame_geometry_from_multiframe_image()`에서는 보존하지만 direct plane helper는
  거부한다.
- NM Image Storage는 NM-specific frame organization을 가지므로 generic enhanced
  stack으로 취급하지 않는다.
