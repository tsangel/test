# DICOM Overlay

当你已经有 source object（例如 SEG frame）和 target image plane 或 volume 的 geometry 时，请使用本指南。Overlay check 只比较已构建的 geometry 和 Frame of Reference UID；它不会重新遍历 dataset，也不会 render pixel。

SEG palette/color rendering、opacity、viewport composition 和 resampling 都是 application 或 viewer 的职责。SEG mask decode 请参见 [DICOM Segmentation (SEG)](segmentation.md)；构建 image/volume geometry 请参见 [DICOM Geometry](geometry.md)。

## 检查 SEG/Image overlay compatibility

Overlay check 使用已经构建好的 geometry 与 frame-of-reference UID。它不会再次遍历数据集，因此可以低成本地重复调用。

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

同一 grid 上的 SEG frame 示例输出：

```text
OverlayCompatibility.compatible
can_transform: True
can_direct_overlay: True
requires_resampling: False
overlaps_extent: True
target_k_range: 35 36
```

这些字段可以这样理解：

- `can_direct_overlay`：grid 足够一致，可以直接按 index copy。
- `requires_resampling`：同一 frame of reference 可用，但 grid mapping 需要 interpolation 或 resampling。
- `different_extent`：grid 一致，但一侧 extent 更小或更大；通常交给 crop、pad 或 clip 策略处理。
- `different_frame_of_reference`：没有外部 registration 时不要 overlay。

当 `can_transform` 为 true 时，先构建一次 transform，再在 paint 或 sampling loop 中复用。

```python
if check.can_transform:
    transform = geom.make_plane_to_volume_transform(seg_plane, target_volume)
    center = geom.ImagePoint2D(seg.columns / 2.0, seg.rows / 2.0)
    print(transform.target_index_from_source_index(center))
```

示例输出：

```text
ImagePoint3D(i=128, j=128, k=35)
```

## 为测试构建 geometry

测试 overlay 的坐标计算不一定需要真实 DICOM 文件。你可以直接构建 geometry 对象。

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

预期输出：

```text
OverlayCompatibility.compatible 4 5
ImagePoint3D(i=10, j=20, k=4)
```

这样可以在 test suite 中不携带真实 patient data 的情况下，对使用 DicomSDL geometry 的应用代码做 unit test。
