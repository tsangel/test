# DICOM Segmentation (SEG)

This page records the public contract for DicomSDL's high-level DICOM Segmentation adapter. In DICOM, Segmentation is the SEG modality (`Modality = SEG`). DicomSDL keeps core dataset reading in `dicom.h` and exposes SEG interpretation through the optional public header `dicom_seg.h`.

## Supported Scope

- SOP Classes: Segmentation Storage (`1.2.840.10008.5.1.4.1.1.66.4`) for BINARY/FRACTIONAL and Label Map Segmentation Storage (`1.2.840.10008.5.1.4.1.1.66.7`) for LABELMAP.
- BINARY SEG: native 1-bit multi-frame PixelData is supported for read/decode. Pixel transcode to or from compressed BINARY SEG is intentionally unsupported until the core pixel layer can represent stored `BitsAllocated=1` layouts end to end.
- FRACTIONAL SEG: 8-bit samples are supported for native uncompressed, Encapsulated Uncompressed, and lossless compressed transfer syntaxes when the corresponding codec is available. Decoding returns raw `uint8` samples; callers can convert with `raw_value / MaximumFractionalValue`.
- LABELMAP SEG: 8-bit and 16-bit stored label samples are supported through Label Map Segmentation Storage for native uncompressed, Encapsulated Uncompressed, and lossless compressed transfer syntaxes when the corresponding codec is available. Decoding preserves stored label values; palette lookup and color rendering are viewer/UI responsibilities.
- Lossy and near-lossless compressed SEG sources and targets are rejected. Big Endian Label Map SEG is unsupported in this contract.
- Metadata views: `SegmentSequence`, `PerFrameFunctionalGroupsSequence`, `SharedFunctionalGroupsSequence`, source-image references, and `FrameOfReferenceUID` are indexed frame by frame.

## Transfer Syntax Support

| PixelData storage | BINARY | FRACTIONAL | LABELMAP |
| --- | --- | --- | --- |
| Native uncompressed Little Endian | read/decode supported | read/write/transcode supported | read/write/transcode supported |
| Native Explicit VR Big Endian | BINARY native read follows the generic DICOM path | supported only where the generic pixel path supports it | unsupported |
| Encapsulated Uncompressed Explicit VR Little Endian | unsupported for BINARY pixel transcode | read/write/transcode supported | read/write/transcode supported |
| Lossless compressed image syntax with registered codec, such as RLE Lossless, JPEG-LS Lossless, JPEG 2000 Lossless, HTJ2K Lossless, or JPEG XL Lossless | unsupported until core 1-bit layout/write support lands | read/write/transcode supported | read/write/transcode supported |
| Lossy or near-lossless compressed syntax | rejected | rejected | rejected |
| Unsupported compressed/video/referenced source codec | rejected at frame decode or transcode time | rejected at frame decode or transcode time | rejected at frame decode or transcode time |

## Required Metadata

The SEG adapter validates the metadata needed for safe frame interpretation by default:

- `FrameOfReferenceUID` is required and is the primary key for deciding whether a SEG can be directly overlaid on another image. `SourceImageSequence` is provenance metadata and does not have to be the only possible display target.
- `Rows`, `Columns`, `SegmentSequence`, and `PerFrameFunctionalGroupsSequence` are required.
- `SharedFunctionalGroupsSequence` must contain exactly one item.
- BINARY and FRACTIONAL frames must resolve to one `ReferencedSegmentNumber`.
- FRACTIONAL SEG must contain `SegmentationFractionalType` and `MaximumFractionalValue`.
- LABELMAP SEG must use Label Map Segmentation Storage with `SegmentationType=LABELMAP`, `BitsAllocated` of 8 or 16, unsigned single-sample pixels, and `PhotometricInterpretation` of `MONOCHROME2` or `PALETTE COLOR`. Stored label values are validated lazily during decode/presence queries or by calling `validate_label_values()`.

When these conditions are not met, the adapter fails loudly instead of returning a misleading mask.

## Pixel Contract

`to_array()` and `decode_frame()` preserve the stored representation. Use `mask_for_segment()` when you need a semantic 0/1 mask for one segment across BINARY, FRACTIONAL, and LABELMAP.

For BINARY SEG, `decode_frame_into()` unpacks one stored 1-bit frame into `uint8` values `0` or `1`:

```cpp
std::vector<std::uint8_t> mask(seg->rows() * seg->columns());
seg->decode_frame_into(frame_index, mask);
// mask values are 0 or 1
```

For FRACTIONAL SEG, `to_array()` returns raw `uint8` stored samples:

```python
raw = seg.to_array(0)  # dtype uint8
fraction = raw.astype("float32") / seg.maximum_fractional_value
```

`mask_for_segment(..., fractional_threshold=...)` produces a semantic binary mask. The default threshold `0.0` means `sample > 0`; other thresholds compare `sample / MaximumFractionalValue >= fractional_threshold`.

For LABELMAP SEG, `to_array()` preserves the stored sample dtype: `uint8` for 8-bit label maps and native-endian `uint16` for 16-bit label maps. `decode_frame()` returns native typed sample bytes, not raw PixelData byte order. `present_segment_numbers(frame)` reports non-background labels actually present in that frame; when `PixelPaddingValue` is present, that segment number is treated as background and excluded. `mask_for_segment(frame, segment_number)` returns a semantic `uint8` 0/1 mask for the requested segment. Unknown stored label values are not checked at file-open time; they are reported when the relevant frame is decoded/scanned or when `validate_label_values()` is called.

LABELMAP presence caches are lazy and thread-safe. A first per-frame presence query may repeat frame-local scan work when called concurrently, but ready cache entries and the all-frame index are immutable and never replaced. All-frame index construction is serialized.

`referenced_segment_number` remains a compatibility accessor for BINARY/FRACTIONAL. LABELMAP frames can contain multiple segment labels, so this accessor raises an error for LABELMAP; use `present_segment_numbers()` and `mask_for_segment()` in common code.

The `_into()` APIs may leave the output buffer partially written if decode or validation raises an exception.

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

The wheel package includes the Python stub via `package_data`, and the CMake target exposes the repository `include/` directory, so `<dicom_seg.h>` is available to builds that consume the source tree. Formal CMake install/export rules are still outside this contract.
