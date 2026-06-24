# DICOM Segmentation (SEG) と Geometry

DICOM Segmentation (SEG) の metadata 確認、SEG frame の decode、image-plane geometry の作成、slice stack の計画、mask と image を安全に重ね合わせられるかの確認に使う API です。

DICOM file では、これらの object は `Modality (0008,0060) = SEG` を使います。より正確な storage 識別子は SOP Class です。BINARY/FRACTIONAL SEG は Segmentation Storage、LABELMAP SEG は Label Map Segmentation Storage を使います。

以下の例は Python で、通常の `dicom.read_file()` ワークフローから始めます。Geometry layer は viewer を作成したり、最終的な出力 volume を確保したり、dominant grid を選んだり、mask を resample したりしません。

mask の例では NumPy 対応を使います。

```bash
pip install "dicomsdl[numpy]"
```

出力値は入力ファイルによって変わります。以下の SEG の出力例は、binary FDG/FBB brain SEG sample から一部を抜粋したものです。

## DICOM Segmentation (SEG) ファイルを開く

DICOM Segmentation Storage と Label Map Segmentation Storage は `dicom.seg.read_file()` で開きます。戻り値は通常の `DicomFile` ではなく `Segmentation` オブジェクトです。

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

出力例:

```text
True
SegmentationType.binary
SegmentationFractionalType.none
None
1.3.6.1.4.1.43046.3.380371456.2303.1779756601.801016
256 256 97 2885
```

すでに byte 列を持っている場合は `read_bytes()` を使います。

```python
data = seg_path.read_bytes()
seg = dicom.seg.read_bytes(data, copy=False)
```

Python で SEG を読むときは `read_file()` または `read_bytes()` から始めます。`dicom.seg.from_dicomfile(df)` は用意していません。既存の Python `DicomFile` から SEG オブジェクトを作るには、大きなデータセットをコピーして再解析する必要があるためです。

DicomSDL は BINARY/FRACTIONAL SEG を Segmentation Storage 経由で、LABELMAP SEG を Label Map Segmentation Storage 経由でサポートします。SEG adapter は open 時点で全 PixelData を scan せず、metadata だけを index します。LABELMAP の stored label value は frame decode/presence scan 時、または明示的に `validate_label_values()` を呼んだ時に検証されます。

## Segment を確認する

DICOM SEG の `SegmentSequence` は、そのオブジェクトに含まれる label を説明します。各 item は 1 つの意味上のクラスで、脳構造、腫瘍、臓器、派生 mask class などに対応します。

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

出力の抜粋:

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

DICOM segment number で直接探すこともできます。

```python
left_white_matter = seg.segment_by_number(1)
if left_white_matter is not None:
    print(left_white_matter.label)
```

出力例:

```text
Left-Cerebral-White-Matter
```

Segment number は Python の list index ではありません。frame と label を対応させるときは `segment.number` を基準にしてください。

## SEG frame を確認する

SEG Pixel Data は multi-frame です。BINARY/FRACTIONAL SEG では、保存された frame は 1 つの referenced segment number に属します。1 つの segment が多くの frame を持ち、各 frame が 1 slice の mask になることがよくあります。

```python
frame = seg.frames[0]

print(frame.index)
print(frame.referenced_segment_number)
print(frame.image_position_patient)
print(frame.image_orientation_patient)
print(frame.pixel_spacing)
print(frame.slice_thickness)
```

出力例:

```text
0
1
(-128.000061, -131.25, -38.999939)
(1.0, 0.0, 0.0, 0.0, 1.0, 0.0)
(1.0, 1.0)
1.0
```

LABELMAP も扱う code では `present_segment_numbers()` を使います。BINARY/FRACTIONAL SEG では宣言された `ReferencedSegmentNumber` を返し、LABELMAP SEG ではその frame に実際に存在する non-background label value を返します。`referenced_segment_number` は互換用 accessor で、LABELMAP frame では定義されません。

```python
print(frame.present_segment_numbers())
```

BINARY frame の出力例:

```text
(1,)
```

Frame には source image reference が含まれることがあります。

```python
for ref in frame.source_images:
    print(ref.sop_class_uid)
    print(ref.sop_instance_uid)
    print(ref.referenced_frame_numbers)
```

出力例:

```text
1.2.840.10008.5.1.4.1.1.2
1.2.840.113619.2.80.981715802.8664.151072595.1914331.90
[]
```

Source image reference は、SEG がどの image から作られたかを示す由来 metadata です。ただし、overlay 先が必ずその image でなければならないという意味ではありません。Overlay ではまず `FrameOfReferenceUID` を比較します。

## SegmentationType ごとの API の選び方

`seg.segmentation_type` は PixelData の保存表現を示します。BINARY、FRACTIONAL、LABELMAP SEG を同じ code で扱う場合は、次の違いを基準にしてください。

| SegmentationType | `to_array()` / `decode_frame()` | segment membership | 推奨 approach |
| --- | --- | --- | --- |
| `binary` | `uint8` mask value `0` または `1` | frame ごとに 1 つの `ReferencedSegmentNumber` | segment 単位の iteration には `frames_for_segment()` を使います。`to_array()` の結果はその frame の semantic mask です。 |
| `fractional` | 保存された raw `uint8` sample | frame ごとに 1 つの `ReferencedSegmentNumber` | threshold mask には `mask_for_segment(..., fractional_threshold=...)` を使い、raw probability/occupancy value が必要な場合は `MaximumFractionalValue` で scaling します。 |
| `labelmap` | 保存された label value、`uint8` または native-endian `uint16` | 1 frame に複数の segment number が入ることがあります | `present_segment_numbers()` と `mask_for_segment()` を使います。LABELMAP frame では `referenced_segment_number` を使わないでください。 |

最も安全な共通 pattern は次の形です。

```python
for frame in seg.frames:
    for segment_number in frame.present_segment_numbers():
        mask = frame.mask_for_segment(segment_number)
        # frame.image_position_patient / geometry mapping と一緒に mask を使います。
```

保存 pixel 表現が必要な場合は `to_array()` を使います。storage type に依存しない semantic `uint8` 0/1 mask が必要な場合は `mask_for_segment()` を使います。LABELMAP では `frames_for_segment()` と `validate_label_values()` が初回呼び出し時に全 frame を scan することがあります。BINARY/FRACTIONAL では open 時点に作成した metadata index を使います。

## BINARY SEG mask を decode する

BINARY SEG は native 1-bit pixel として DICOM に保存されます。DicomSDL はこれを unpack し、値が `0` または `1` の `uint8` mask を返します。

```python
mask = seg.to_array(0)
print(mask.shape, mask.dtype)
print(mask.min(), mask.max())
```

出力例:

```text
(256, 256) uint8
0 1
```

1 つの segment に属する frame をすべて decode します。

```python
masks_for_segment = []

for frame in seg.frames_for_segment(1):
    mask = frame.to_array()
    masks_for_segment.append((frame.index, mask))

print("decoded frames:", len(masks_for_segment))
```

出力例:

```text
decoded frames: 87
```

出力配列を再利用したい場合は `decode_frame_into()` を使います。

```python
import numpy as np

out = np.empty((seg.rows, seg.columns), dtype=np.uint8)
returned = seg.decode_frame_into(0, out)
print(returned is out)
print(out.shape, out.dtype, out.min(), out.max())
```

出力例:

```text
True
(256, 256) uint8 0 1
```

## FRACTIONAL SEG mask を decode する

FRACTIONAL SEG は 8-bit raw sample を保存します。DicomSDL は raw sample をそのまま返し、scaling は呼び出し側で明示的に行います。

```python
import numpy as np

if seg.segmentation_type is dicom.seg.SegmentationType.fractional:
    if not seg.maximum_fractional_value:
        raise ValueError("MaximumFractionalValue is missing")

    raw = seg.to_array(0)  # dtype uint8
    values = raw.astype(np.float32) / float(seg.maximum_fractional_value)
    print(values.min(), values.max())
```

上の sample は BINARY SEG なので、この FRACTIONAL block は何も出力しません。Raw 値が保存範囲全体に広がる FRACTIONAL SEG では、次のような出力になります。

```text
0.0 1.0
```

こうしておくと、probability、occupancy、thresholded boolean mask などの後段処理で、必要な精度とデータ配置を呼び出し側が選べます。

## LABELMAP SEG frame を decode する

LABELMAP SEG は label value を PixelData に直接保存します。Label value `0` は background で、0 以外の値は `SegmentSequence` の segment number に対応します。DicomSDL は保存表現を保ちます。8-bit label map は `uint8`、16-bit label map は native-endian `uint16` として decode されます。

```python
if seg.segmentation_type is dicom.seg.SegmentationType.labelmap:
    labels = seg.to_array(0)
    print(labels.shape, labels.dtype)
    print(seg.present_segment_numbers(0))
```

出力例:

```text
(512, 512) uint16
(1, 24, 300)
```

Palette lookup、color mapping、opacity、legend rendering は viewer/UI layer の責任です。DicomSDL は stored label sample と metadata を返し、palette image は描画しません。

特定の segment の semantic mask が必要な場合は `mask_for_segment()` を使います。この API は BINARY、FRACTIONAL、LABELMAP SEG で共通に動作します。FRACTIONAL SEG では threshold が normalized `[0, 1]` 単位で適用されます。

```python
segment_number = 24
mask = seg.mask_for_segment(0, segment_number)
print(mask.shape, mask.dtype, mask.min(), mask.max())
```

出力例:

```text
(512, 512) uint8 0 1
```

`present_segment_numbers(frame)` は指定された LABELMAP frame だけを scan して結果を cache します。`frames_for_segment(segment_number)` と `validate_label_values()` は初回呼び出し時に全 LABELMAP frame を scan することがあるため、大きな multi-frame SEG では明示的な validation/indexing 操作として扱ってください。

## DICOM plane tag を geometry に変換する

Geometry module は DICOM image plane metadata を検証済みの geometry オブジェクトに変換します。

```python
g = dicom.geometry

image = dicom.read_file(r"C:\data\CT-slice-0035.dcm")
plane = g.plane_from_single_frame_image(image)

world = plane.world_from_index(g.ImagePoint2D(100.0, 120.0))
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

## SEG/Image overlay compatibility を確認する

Overlay check は、すでに作成済みの geometry と frame-of-reference UID だけを使います。Dataset を再走査しないため、低コストで繰り返し呼び出せます。

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

同じ grid 上の SEG frame では、次のような出力になります。

```text
OverlayCompatibility.compatible
can_transform: True
can_direct_overlay: True
requires_resampling: False
overlaps_extent: True
target_k_range: 35 36
```

主なフィールドは次のように読みます。

- `can_direct_overlay`: grid が十分一致しており、直接 index copy できる。
- `requires_resampling`: 同じ frame of reference にあるが、grid mapping に interpolation または resampling が必要。
- `different_extent`: grid は一致するが、片方の extent が小さい、または大きい。通常は crop、pad、clip の方針で扱う問題。
- `different_frame_of_reference`: 外部 registration なしで overlay してはいけない。

`can_transform` が true の場合、transform を 1 回だけ作り、paint loop や sampling loop で再利用します。

```python
if check.can_transform:
    transform = g.make_plane_to_volume_transform(seg_plane, target_volume)
    center = g.ImagePoint2D(seg.columns / 2.0, seg.rows / 2.0)
    print(transform.target_index_from_source_index(center))
```

出力例:

```text
ImagePoint3D(i=128, j=128, k=35)
```

## Enhanced multi-frame image を扱う

Enhanced CT/MR/PET は 1 つのファイルに多くの frame を保存できます。1 つのファイルの中に複数の stack、time point、phase、echo が混在する場合があるため、最初に frame を group に分けます。

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
single_plan = g.plan_image_frame_stack(file)
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

- `missing_geometry`: 必要な plane tag を解決できない。
- `missing_dimension_module`: enhanced grouping metadata がない。
- `multiple_frame_stacks`: ファイル全体を対象にした convenience call で複数 stack が見つかった。
- `duplicate_slice_position`: 2 つの frame が同じ slice position にある。
- `non_uniform_spacing`: stack は解析できるが、1 つの uniform volume grid ではない。
- `inconsistent_slice_origin`: slice origin が in-plane にずれており、1 つの rectilinear affine volume として表せない。

Non-uniform input でも `uniform_runs` は有用な連続部分範囲を示します。DicomSDL はそれを報告しますが、dominant grid を選んだり、その grid に resample したりはしません。

## テスト用の geometry を作る

Overlay の座標計算をテストするために、実際の DICOM ファイルは必須ではありません。Geometry オブジェクトは直接作れます。

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

期待される出力:

```text
OverlayCompatibility.compatible 4 5
ImagePoint3D(i=10, j=20, k=4)
```

実際の patient data を test suite に入れずに、DicomSDL geometry を使うアプリケーションコードを unit test するのに便利です。
