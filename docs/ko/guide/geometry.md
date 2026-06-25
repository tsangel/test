# DICOM Geometry

DicomSDL은 `ImagePositionPatient`, `ImageOrientationPatient`, `PixelSpacing`,
`Rows`, `Columns` 같은 DICOM Image Plane attributes에서 geometry 객체를
만듭니다.

`ImagePlaneGeometry`는 plane origin, row/column direction, normal, pixel
spacing, image size, index/world transform을 담습니다.
`ImageVolumeGeometry`는 여기에 slice direction, slice spacing, slice count,
3D index/world transform을 더해 rectilinear volume geometry를 표현합니다.

## Image Plane attributes로 geometry 만들기

Geometry module은 DICOM Image Plane attributes를 검증된 객체로 바꿔준다.

```python
geom = dicom.geometry

image = dicom.read_file(r"C:\data\CT-slice-0035.dcm")
plane = geom.plane_from_single_frame_image(image)

world = plane.world_from_index(geom.ImagePoint2D(100.0, 120.0))
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

## Enhanced multi-frame image

Enhanced CT/MR/PET은 한 파일 안에 많은 frame을 담을 수 있다. 한 파일 안에 여러
stack, time point, phase, echo가 섞일 수도 있으므로 먼저 frame을 group으로 나눈다.

```python
geom = dicom.geometry
file = dicom.read_file(r"C:\data\enhanced-ct.dcm")

frame_geometry = geom.frame_geometry_from_multiframe_image(file, 0)
print("frame kind:", frame_geometry.kind)

vol_props = geom.volumetric_properties_from_multiframe_image(file, 0)
print("volumetric properties:", vol_props.value)

stacks = geom.analyze_image_frame_stacks(file)
print("status:", stacks.status)
print("groups:", len(stacks.groups))

for group_index, group in enumerate(stacks.groups):
    print("group:", group_index)
    print("stack_id:", group.key.stack_id)
    print("frames:", group.frame_indices)
    print("analysis:", group.analysis.status)

    if group.analysis.ok:
        plan = geom.plan_image_frame_stack(file, group.frame_indices)
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
single_plan = geom.plan_image_frame_stack(file)
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

- `missing_geometry`: 필요한 Image Plane attributes를 찾지 못했다.
- `missing_dimension_module`: enhanced grouping metadata가 없다.
- `multiple_frame_stacks`: 파일 전체 convenience call에서 stack이 둘 이상 발견됐다.
- `duplicate_slice_position`: 두 frame이 같은 slice 위치에 있다.
- `non_uniform_spacing`: stack은 분석할 수 있지만 하나의 uniform volume grid는 아니다.
- `inconsistent_slice_origin`: slice origin이 in-plane으로 흔들려 하나의 rectilinear
  affine volume으로 표현할 수 없다.

Non-uniform input에서도 `uniform_runs`는 유용한 연속 sub-range를 알려준다.
DicomSDL은 이를 보고하지만 dominant grid를 고르거나 그 grid로 resampling하지는
않는다.
