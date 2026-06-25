# DICOM Segmentation MVP

This page records the first DicomSDL contract for the high-level DICOM SEG adapter. The design keeps core DICOM reading in `dicom.h` and exposes SEG interpretation through the optional public header `dicom_seg.h`.

## Supported Scope

- SOP Classes: Segmentation Storage (`1.2.840.10008.5.1.4.1.1.66.4`) for BINARY/FRACTIONAL and Label Map Segmentation Storage (`1.2.840.10008.5.1.4.1.1.66.7`) for LABELMAP.
- BINARY SEG: native 1-bit multi-frame PixelData is supported. `decode_frame_into()` unpacks one stored frame into one byte per pixel, with values `0` or `1`.
- FRACTIONAL SEG: native 8-bit PixelData is supported. Decoding returns raw `uint8` samples. Callers can convert to fractional values with `raw_value / MaximumFractionalValue`.
- LABELMAP SEG: native uncompressed 8-bit or 16-bit stored label samples are supported through Label Map Segmentation Storage. Decoding preserves the stored label values; palette lookup and color rendering are left to viewer/UI code.
- Metadata views: `SegmentSequence`, `PerFrameFunctionalGroupsSequence`, `SharedFunctionalGroupsSequence`, source-image references, and `FrameOfReferenceUID` are indexed frame by frame.

## Post-MVP

- Volume reconstruction APIs that assemble frame masks into 3D arrays.
- Affine or overlay helpers that map SEG frames onto display images.
- Compressed or encapsulated SEG PixelData, including RLE or other encapsulated transfer syntaxes.

## Required Metadata

The SEG adapter validates the metadata needed by this MVP by default:

- `FrameOfReferenceUID` is required and is the primary key for deciding whether a SEG can be directly overlaid on another image. `SourceImageSequence` is provenance metadata and does not have to be the only possible display target.
- `Rows`, `Columns`, `SegmentSequence`, and `PerFrameFunctionalGroupsSequence` are required.
- `SharedFunctionalGroupsSequence` must contain exactly one item.
- BINARY and FRACTIONAL frames must resolve to one `ReferencedSegmentNumber`.
- FRACTIONAL SEG must contain `SegmentationFractionalType` and `MaximumFractionalValue`.
- LABELMAP SEG must use Label Map Segmentation Storage with `SegmentationType=LABELMAP`, `BitsAllocated` of 8 or 16, unsigned single-sample pixels, and `PhotometricInterpretation` of `MONOCHROME2` or `PALETTE COLOR`. Stored label values are validated lazily during decode/presence queries or by calling `validate_label_values()`.

When these conditions are not met, the adapter should fail loudly instead of returning a misleading mask.

## Pixel Contract

For BINARY SEG, the MVP supports native 1-bit DICOM PixelData. The public API always returns a decoded 8-bit frame, not packed bits:

```cpp
std::vector<std::uint8_t> mask(seg->rows() * seg->columns());
seg->decode_frame_into(frame_index, mask);
// mask values are 0 or 1
```

For FRACTIONAL SEG, the MVP returns the raw 8-bit stored sample:

```python
raw = seg.to_array(0)  # dtype uint8
fraction = raw.astype("float32") / seg.maximum_fractional_value
```

The scaling step stays with the caller so probability and occupancy consumers can choose their output precision and memory layout.

For LABELMAP SEG, `to_array()` preserves the stored sample dtype: `uint8` for 8-bit label maps and native-endian `uint16` for 16-bit label maps. `present_segment_numbers(frame)` reports non-background labels actually present in that frame; when `PixelPaddingValue` is present, that segment number is treated as background and excluded. `mask_for_segment(frame, segment_number)` returns a semantic `uint8` 0/1 mask for the requested segment. Unknown stored label values are not checked at file-open time; they are reported when the relevant frame is decoded/scanned or when `validate_label_values()` is called.

## API Pattern

C++ code normally uses the SEG convenience readers. Advanced callers that already own a parsed `DicomFile` can move it into the SEG adapter with `from_dicomfile()`:

```cpp
#include <dicom.h>
#include <dicom_seg.h>

auto seg = dicom::seg::read_file(path);

auto file = dicom::read_file(path);
auto seg_from_file = dicom::seg::from_dicomfile(std::move(file));
```

The C++ adapter owns the `DicomFile`; returned segment and frame views borrow from it. This avoids copying strings and DICOM items while keeping view lifetimes simple.

Python uses the same naming:

```python
import dicomsdl as dicom

seg = dicom.seg.read_file(path)
seg = dicom.seg.read_bytes(data, copy=False)
```

There is no Python `dicom.seg.from_dicomfile(df)` helper. Python cannot move ownership out of an existing `DicomFile` object without copying the full dataset, which is too easy to choose accidentally for large SEG instances.

## Regression Tests

The repository keeps synthetic BINARY, FRACTIONAL, and LABELMAP SEG tests in C++ and Python. A local real-sample regression can be enabled without committing private data:

```powershell
$env:DICOMSDL_SEG_SAMPLE_PATH = "C:\path\to\sample-seg.dcm"
python -m pytest tests/python/test_segmentation.py -q
```

The wheel package includes the Python stub via `package_data`, and the CMake target exposes the repository `include/` directory, so `<dicom_seg.h>` is available to builds that consume the source tree. Formal CMake install/export rules are still outside this MVP.
