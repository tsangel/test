# DICOM Segmentation (SEG)

Use this guide when you need to inspect DICOM Segmentation (SEG) metadata, decode SEG frames, or build semantic masks from BINARY, FRACTIONAL, or LABELMAP SEG objects.

In DICOM files, these objects use `Modality (0008,0060) = SEG`. The precise storage identifier is the SOP Class: Segmentation Storage for BINARY and FRACTIONAL SEG, and Label Map Segmentation Storage for LABELMAP SEG.

Install NumPy support for the mask examples:

```bash
pip install "dicomsdl[numpy]"
```

Output values depend on the input file. The SEG excerpts below are adapted from one binary FDG/FBB brain SEG sample.

## Open a DICOM Segmentation (SEG) File

Open DICOM Segmentation Storage or Label Map Segmentation Storage with
`dicom.seg.read_file()`. It returns a `Segmentation` object instead of a plain
`DicomFile`.

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

Example output:

```text
True
SegmentationType.binary
SegmentationFractionalType.none
SegmentsOverlap.undefined
None
1.3.6.1.4.1.43046.3.380371456.2303.1779756601.801016
256 256 97 2885
```

If you already have bytes, use `read_bytes()`:

```python
data = seg_path.read_bytes()
seg = dicom.seg.read_bytes(data, copy=False)
```

In Python, SEG input starts with `read_file()` or `read_bytes()`. There is no
`dicom.seg.from_dicomfile(df)` entry point because it would need to copy and
reparse the existing `DicomFile`.

DicomSDL supports BINARY and FRACTIONAL SEG through Segmentation Storage, and
LABELMAP SEG through Label Map Segmentation Storage. The adapter opens SEG
metadata without scanning the entire PixelData element up front; LABELMAP stored
label values are validated when frames are decoded or scanned, or when
`validate_label_values()` is called explicitly.

## Inspect Segments

In DICOM SEG, `SegmentSequence` describes the labels carried by the object. Each
item is one semantic class, such as a brain structure, tumor, organ, or derived
mask class.

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

Output excerpt:

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

You can also look up a segment by its DICOM segment number:

```python
left_white_matter = seg.segment_by_number(1)
if left_white_matter is not None:
    print(left_white_matter.label)
```

Example output:

```text
Left-Cerebral-White-Matter
```

Segment numbers are not the same as Python list indices. Prefer
`segment.number` when you are matching frames to labels.

## Inspect SEG Frames

SEG Pixel Data is multi-frame. For BINARY and FRACTIONAL SEG, each stored frame
belongs to one referenced segment number. A segment usually has many frames,
often one per stored slice.

```python
frame = seg.frames[0]

print(frame.index)
print(frame.referenced_segment_number)
print(frame.image_position_patient)
print(frame.image_orientation_patient)
print(frame.pixel_spacing)
print(frame.slice_thickness)
```

Example output:

```text
0
1
(-128.000061, -131.25, -38.999939)
(1.0, 0.0, 0.0, 0.0, 1.0, 0.0)
(1.0, 1.0)
1.0
```

Use `present_segment_numbers()` when you want code that also works for
LABELMAP SEG. For BINARY and FRACTIONAL SEG it returns the declared
`ReferencedSegmentNumber`; for LABELMAP SEG it returns the non-background label
values actually present in that frame. `referenced_segment_number` is a
compatibility accessor and is not defined for LABELMAP frames.

```python
print(frame.present_segment_numbers())
```

Example output for a BINARY frame:

```text
(1,)
```

Frames can also contain source image references:

```python
for ref in frame.source_images:
    print(ref.sop_class_uid)
    print(ref.sop_instance_uid)
    print(ref.referenced_frame_numbers)
```

Example output:

```text
1.2.840.10008.5.1.4.1.1.2
1.2.840.113619.2.80.981715802.8664.151072595.1914331.90
[]
```

Source image references are provenance metadata. They tell you which images were
used to create the SEG, but they are not the only possible display target. For
overlay, compare `FrameOfReferenceUID` first.

## Choose APIs by SegmentationType

`seg.segmentation_type` tells you the stored representation. For code that works
across BINARY, FRACTIONAL, and LABELMAP SEG, keep this split in mind:

| SegmentationType | `to_array()` / `decode_frame()` | Segment membership | Recommended approach |
| --- | --- | --- | --- |
| `binary` | `uint8` mask values `0` or `1` | One `ReferencedSegmentNumber` per frame | Use `frames_for_segment()` for segment-wise iteration. `to_array()` is already a semantic mask for that frame. |
| `fractional` | Raw `uint8` stored samples | One `ReferencedSegmentNumber` per frame | Use `mask_for_segment(..., fractional_threshold=...)` for thresholded masks, or scale raw samples by `MaximumFractionalValue` yourself. |
| `labelmap` | Stored label values, `uint8` or native-endian `uint16` | A frame may contain multiple segment numbers | Use `present_segment_numbers()` and `mask_for_segment()`. Do not use `referenced_segment_number` for LABELMAP frames. |

The safest general pattern is:

```python
for frame in seg.frames:
    for segment_number in frame.present_segment_numbers():
        mask = frame.mask_for_segment(segment_number)
        # Use mask with frame.image_position_patient / geometry mapping.
```

Use `to_array()` when you need the stored pixel representation. Use
`mask_for_segment()` when you need a semantic `uint8` 0/1 mask independent of
storage type. For LABELMAP, `frames_for_segment()` and `validate_label_values()`
may scan all frames on first use; for BINARY and FRACTIONAL they use the
metadata index built at open time.

## Decode a BINARY SEG Mask

For BINARY SEG, DICOM stores 1-bit pixels. DicomSDL returns an unpacked
`uint8` mask with values `0` and `1`. Native uncompressed and Encapsulated
Uncompressed BINARY SEG can be read this way. In C++, callers that need to
avoid unpacking a whole frame can use `binary_frame_bits()` with
`for_each_binary_frame_set_bit()`.

```python
mask = seg.to_array(0)
print(mask.shape, mask.dtype)
print(mask.min(), mask.max())
```

Example output:

```text
(256, 256) uint8
0 1
```

Decode all frames for one segment:

```python
masks_for_segment = []

for frame in seg.frames_for_segment(1):
    mask = frame.to_array()
    masks_for_segment.append((frame.index, mask))

print("decoded frames:", len(masks_for_segment))
```

Example output:

```text
decoded frames: 87
```

If you want to reuse an output array:

```python
import numpy as np

out = np.empty((seg.rows, seg.columns), dtype=np.uint8)
returned = seg.decode_frame_into(0, out)
print(returned is out)
print(out.shape, out.dtype, out.min(), out.max())
```

Example output:

```text
True
(256, 256) uint8 0 1
```

## Pack BINARY SEG Frames as One Label Volume

The main motivation is a BINARY SEG object with many segments that rarely
overlap. When building an application such as a viewer, uploading one 3D mask
texture per segment wastes memory and GPU binding work in that case. DicomSDL
can pack those BINARY SEG frames into one `uint16` label volume. The application
still chooses the target grid and maps each SEG frame to a slice index.

```python
import numpy as np

# Produced by the image geometry / stack chosen by your application.
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

Example output:

```text
(127, 256, 256) uint16
[0, 1, 2, 3]
```

`labels` has shape `(slices, rows, columns)`. Code `0` is background. Other
codes are runtime label codes; some codes can represent an overlap of two or
more segments. Use the returned `BinaryLabelVolume` object when you need
semantic membership:

```python
segment_number = 2
label_id = packed.label_id_for_segment_number(segment_number)
mask = packed.restore_mask_for_segment(segment_number)

code = int(labels[30, 120, 90])
print(packed.label_set(code))
```

`BinaryLabelVolume` also stores per-label voxel statistics while it builds the
volume. The coordinates are voxel indices in `(x, y, z)` order, where `x` is
column, `y` is row, and `z` is slice:

```python
stats = packed.label_stats_for_segment(segment_number)
print(stats.voxel_count)
print(stats.min_index, stats.max_index)
print(stats.centroid_index)
```

For a display LUT, provide a dense `label_id -> RGBA` table. The returned array
has shape `(label_code_count, 4)` and is indexed by the stored label code.
Overlap label-code colors are averaged from their member labels; applications
that need a different blend policy can use `label_set()` and build their own
LUT. If a renderer wants a 2D texture, padding or reshaping this table belongs
to the application.

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
assert rgba_lut.shape == (packed.label_code_count, 4)
```

For viewer pipelines that already own a staging buffer, use the allocation-free
variant:

```python
label_volume = np.empty((slice_count, seg.rows, seg.columns), dtype=np.uint16)

packed = seg.build_binary_label_volume_into(
    label_volume,
    frame_placements,
    slices=slice_count,
)

assert np.shares_memory(packed.label_volume, label_volume)
```

`build_binary_label_volume_into()` is the preferred path when an application
wants to control CPU/GPU staging allocation. Visibility, colors, opacity, and
lookup-table rendering remain viewer responsibilities.

## Decode a FRACTIONAL SEG Mask

FRACTIONAL SEG stores 8-bit raw samples. DicomSDL returns those raw samples and
leaves scaling to the caller.

```python
import numpy as np

if seg.segmentation_type is dicom.seg.SegmentationType.fractional:
    if not seg.maximum_fractional_value:
        raise ValueError("MaximumFractionalValue is missing")

    raw = seg.to_array(0)  # dtype uint8
    values = raw.astype(np.float32) / float(seg.maximum_fractional_value)
    print(values.min(), values.max())
```

The sample output above is from a BINARY SEG, so this FRACTIONAL block prints
nothing for that file. On a FRACTIONAL SEG whose raw values span the full stored
range, the output would look like:

```text
0.0 1.0
```

This keeps probability and occupancy workflows explicit. The caller can choose
`float32`, `float64`, thresholded boolean masks, or another downstream layout.

## Decode a LABELMAP SEG Frame

LABELMAP SEG stores label values directly in PixelData. Stored values
correspond to `SegmentSequence` segment numbers. When `PixelPaddingValue` is
present, that segment number is treated as background and is excluded from
`present_segment_numbers()`. DicomSDL preserves the stored representation:
8-bit label maps decode to `uint8`, and 16-bit label maps decode to
native-endian `uint16`.

```python
if seg.segmentation_type is dicom.seg.SegmentationType.labelmap:
    labels = seg.to_array(0)
    print(labels.shape, labels.dtype)
    print(seg.present_segment_numbers(0))
```

Example output:

```text
(512, 512) uint16
(1, 24, 300)
```

Palette lookup, color mapping, opacity, and legend rendering are viewer/UI
responsibilities. DicomSDL returns stored label samples and metadata; it does
not render a palette image.

To get a semantic mask for one segment, use `mask_for_segment()`. This API works
across BINARY, FRACTIONAL, and LABELMAP SEG. For FRACTIONAL SEG, the threshold
is applied in normalized `[0, 1]` units.

```python
segment_number = 24
mask = seg.mask_for_segment(0, segment_number)
print(mask.shape, mask.dtype, mask.min(), mask.max())
```

Example output:

```text
(512, 512) uint8 0 1
```

`present_segment_numbers(frame)` scans only the requested LABELMAP frame and
caches the result. `frames_for_segment(segment_number)` may scan all LABELMAP
frames on first use. `validate_label_values()` validates LABELMAP labels across
all frames and decodes FRACTIONAL frames to check `MaximumFractionalValue`, so
treat it as an explicit validation operation for large multi-frame SEG objects.
