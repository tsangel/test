# DICOM Overlay

SEG frame などの source object と target image plane または volume の geometry が既にあるときはこのガイドを使います。Overlay check は構築済みの geometry と Frame of Reference UID だけを比較し、dataset を再走査したり pixel を render したりしません。

SEG palette/color rendering、opacity、viewport composition、resampling は application または viewer の責務です。SEG mask decode は [DICOM Segmentation (SEG)](segmentation.md)、image/volume geometry の構築は [DICOM Geometry](geometry.md) を参照してください。

## SEG/Image overlay compatibility を確認する

Overlay check は、すでに作成済みの geometry と frame-of-reference UID だけを使います。Dataset を再走査しないため、低コストで繰り返し呼び出せます。

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
    transform = geom.make_plane_to_volume_transform(seg_plane, target_volume)
    center = geom.ImagePoint2D(seg.columns / 2.0, seg.rows / 2.0)
    print(transform.target_index_from_source_index(center))
```

出力例:

```text
ImagePoint3D(i=128, j=128, k=35)
```

## テスト用の geometry を作る

Overlay の座標計算をテストするために、実際の DICOM ファイルは必須ではありません。Geometry オブジェクトは直接作れます。

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

期待される出力:

```text
OverlayCompatibility.compatible 4 5
ImagePoint3D(i=10, j=20, k=4)
```

実際の patient data を test suite に入れずに、DicomSDL geometry を使うアプリケーションコードを unit test するのに便利です。
