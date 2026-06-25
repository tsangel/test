# DICOM Geometry

DicomSDL は `ImagePositionPatient`、`ImageOrientationPatient`、`PixelSpacing`、
`Rows`、`Columns` などの DICOM Image Plane attributes から geometry object
を作ります。

`ImagePlaneGeometry` は plane origin、row/column direction、normal、pixel
spacing、image size、index/world transform を持ちます。
`ImageVolumeGeometry` はさらに slice direction、slice spacing、slice
count、3D index/world transform を持ち、rectilinear volume geometry を表します。

## Image Plane attributes から geometry を作る

Geometry module は DICOM Image Plane attributes を検証済みの geometry オブジェクトに変換します。

```python
geom = dicom.geometry

image = dicom.read_file(r"C:\data\CT-slice-0035.dcm")
plane = geom.plane_from_single_frame_image(image)

world = plane.world_from_index(geom.ImagePoint2D(100.0, 120.0))
index = plane.index_from_world(world)

print(world)
print(index)
```

1 mm axial slice の出力例:

```text
Point3d(x=-28.000061, y=-11.25, z=-38.999939)
ImagePoint2D(i=100, j=120)
```

DicomSDL では index 名を次のように使います。

- `i`: column 方向の image index。
- `j`: row 方向の image index。
- `k`: volume の slice/frame index。

DICOM `PixelSpacing` は `[row_spacing, column_spacing]` の順です。DicomSDL はこれを `spacing_j` と `spacing_i` に分けます。

```python
print("columns, rows:", plane.columns, plane.rows)
print("spacing_i, spacing_j:", plane.spacing_i, plane.spacing_j)
```

出力例:

```text
columns, rows: 512 512
spacing_i, spacing_j: 1.0 1.0
```

## Classic slice stack を計画する

Classic CT/MR/PET series は、slice ごとに 1 つの SOP instance を持つことがよくあります。`plan_slice_stack()` はそれらのファイルを物理 slice 順に並べ、volume geometry と placement list を返します。

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

出力例:

```text
256 256 91
1.0 1.0 1.0
source file 0 frame 0 -> k 0
source file 1 frame 0 -> k 1
source file 2 frame 0 -> k 2
source file 3 frame 0 -> k 3
source file 4 frame 0 -> k 4
```

pixel volume を自分で組み立てる場合は placement を使います。

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

出力例:

```text
(91, 256, 256) uint16
```

Geometry layer は pixel を decode したり、出力 volume を確保したりしません。viewer、batch job、resampling pipeline がそれぞれの memory layout と方針を選べるようにするためです。

## Enhanced multi-frame image を扱う

Enhanced CT/MR/PET は 1 つのファイルに多くの frame を保存できます。1 つのファイルの中に複数の stack、time point、phase、echo が混在する場合があるため、最初に frame を group に分けます。

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

単一の enhanced CT stack では、次のような出力になります。

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

`plane_from_multiframe_image(file, frame_index)` は direct overlay 用の helper で、regular slice plane だけを返します。`frame_geometry_from_multiframe_image()` は `ImageFrameGeometryKind` を保持するため、sampled projection や distorted frame metadata を確認しつつ、それらを通常の slice として誤って扱うことを避けられます。

ファイル全体が 1 つの stack だと分かっている場合だけ `plan_image_frame_stack(file)` を直接使います。複数 group があり得る場合は、group ごとの frame list を明示して plan してください。

```python
single_plan = geom.plan_image_frame_stack(file)
if not single_plan.ok:
    for issue in single_plan.issues:
        print(issue.status, issue.frame_index, issue.tag, issue.message)
```

ファイルに複数の stack が含まれる場合、出力は次のようになります。

```text
SliceStackStatus.multiple_frame_stacks 0 (0020,9157) file contains multiple frame stacks
```

## Reconstructed NM TOMO stack を扱う

Nuclear Medicine Image Storage は、Enhanced image とは異なる古い frame organization tag を使います。DicomSDL には reconstructed TOMO stack 用の adapter があります。

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

出力例:

```text
128 128 64
[(0, 0), (1, 1), (2, 2), (3, 3), (4, 4), (5, 5), (6, 6), (7, 7), (8, 8), (9, 9)]
```

現在の NM adapter は、`RECON TOMO` と `RECON GATED TOMO` のうち、`NumberOfFrames` があり、`FrameIncrementPointer` が正確に 1 つの値 `SliceVector` だけを持つ場合を受け入れます。Projection acquisition や、time/energy などの追加 vector が混在する場合は、推測せずに拒否します。

## Stack failure を診断する

Analysis オブジェクトは構造化された issue を保持するため、viewer や batch script が次の動作を決めやすくなります。

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

Non-uniform stack の出力例:

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

よく見る status は次のとおりです。

- `missing_geometry`: 必要な Image Plane attributes を解決できない。
- `missing_dimension_module`: enhanced grouping metadata がない。
- `multiple_frame_stacks`: ファイル全体を対象にした convenience call で複数 stack が見つかった。
- `duplicate_slice_position`: 2 つの frame が同じ slice position にある。
- `non_uniform_spacing`: stack は解析できるが、1 つの uniform volume grid ではない。
- `inconsistent_slice_origin`: slice origin が in-plane にずれており、1 つの rectilinear affine volume として表せない。

Non-uniform input でも `uniform_runs` は有用な連続部分範囲を示します。DicomSDL はそれを報告しますが、dominant grid を選んだり、その grid に resample したりはしません。
