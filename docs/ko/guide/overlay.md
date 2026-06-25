# DICOM Overlay

SEG frame 같은 source 객체와 target image plane 또는 volume의 geometry가 이미 있을 때 이 가이드를 사용하세요. Overlay check는 이미 만들어진 geometry와 Frame of Reference UID만 비교하며, dataset을 다시 순회하거나 pixel을 render하지 않습니다.

SEG palette/color rendering, opacity, viewport composition, resampling은 application 또는 viewer의 책임입니다. SEG mask decode는 [DICOM Segmentation(SEG)](segmentation.md)를, image/volume geometry 생성은 [DICOM Geometry](geometry.md)를 보세요.

## SEG/Image overlay compatibility

Overlay check는 이미 만들어진 geometry 객체와 frame of reference UID만 본다.
Dataset을 다시 순회하지 않으므로 가볍게 반복 호출할 수 있다.

```python
from pathlib import Path

geom = dicom.geometry

seg = dicom.seg.read_file(r"C:\data\sample-seg.dcm")
image_files = [dicom.read_file(path) for path in sorted(Path(r"C:\data\CT").glob("*.dcm"))]
image_plan = geom.plan_slice_stack(image_files)
if not image_plan.ok or image_plan.volume_geometry is None:
    raise ValueError(f"target image stack is not a uniform volume: {image_plan.status}")

seg_frame = seg.frames[0]
seg_plane = geom.plane_from_seg_frame(seg, seg_frame.index)
target_volume = image_plan.volume_geometry

check = geom.check_overlay_compatibility(
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
    transform = geom.make_plane_to_volume_transform(seg_plane, target_volume)
    center = geom.ImagePoint2D(seg.columns / 2.0, seg.rows / 2.0)
    print(transform.target_index_from_source_index(center))
```

예시 출력:

```text
ImagePoint3D(i=128, j=128, k=35)
```

## 테스트용 synthetic geometry 만들기

Overlay math를 테스트하는 데 실제 DICOM 파일이 꼭 필요하지는 않다. Geometry 객체를
직접 만들 수 있다.

```python
geom = dicom.geometry

seg_plane = geom.make_image_plane_geometry(
    geom.Point3d(0.0, 0.0, 20.0),
    geom.Vec3d(1.0, 0.0, 0.0),
    geom.Vec3d(0.0, 1.0, 0.0),
    geom.ImageSpacing2D(1.0, 1.0),
    geom.ImageSize2D(64, 64),
)

image_volume = geom.make_image_volume_geometry(
    geom.Point3d(0.0, 0.0, 0.0),
    geom.Vec3d(1.0, 0.0, 0.0),
    geom.Vec3d(0.0, 1.0, 0.0),
    geom.Vec3d(0.0, 0.0, 1.0),
    geom.ImageSpacing3D(1.0, 1.0, 5.0),
    geom.ImageSize3D(64, 64, 16),
)

check = geom.check_overlay_compatibility("1.2.3", seg_plane, "1.2.3", image_volume)
if check.target_k_range is not None:
    print(check.status, check.target_k_range.begin, check.target_k_range.end)

transform = geom.make_plane_to_volume_transform(seg_plane, image_volume)
print(transform.target_index_from_source_index(geom.ImagePoint2D(10.0, 20.0)))
```

예상 출력:

```text
OverlayCompatibility.compatible 4 5
ImagePoint3D(i=10, j=20, k=4)
```

이 방식은 실제 patient data를 테스트 suite에 넣지 않고도 DicomSDL geometry를
사용하는 application 코드를 unit test하기에 좋다.
