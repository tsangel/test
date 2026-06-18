# DICOM Segmentation MVP

This page records the first DicomSDL contract for the high-level DICOM SEG adapter. Core DICOM reading stays in `dicom.h`; SEG interpretation is exposed by the optional public header `dicom_seg.h`.

## Supported Scope

- SOP Class: Segmentation Storage (`1.2.840.10008.5.1.4.1.1.66.4`) only.
- BINARY SEG: native 1-bit multi-frame PixelData. The API unpacks one frame into `uint8` values `0` or `1`.
- FRACTIONAL SEG: native 8-bit PixelData. The API returns raw `uint8` samples; callers scale with `raw_value / MaximumFractionalValue`.
- Metadata views index `SegmentSequence`, `PerFrameFunctionalGroupsSequence`, `SharedFunctionalGroupsSequence`, source-image references, and `FrameOfReferenceUID`.

## Post-MVP

- LABELMAP SEG and Label Map Segmentation Storage.
- Volume reconstruction APIs.
- Affine or overlay helpers.
- Compressed or encapsulated BINARY SEG PixelData.

## API Pattern

C++ code reads with the core API and moves ownership into SEG:

```cpp
#include <dicom.h>
#include <dicom_seg.h>

auto file = dicom::read_file(path);
auto seg = dicom::seg::from_dicomfile(std::move(file));
```

Python users should prefer `dicom.seg.from_file()` and `dicom.seg.from_bytes()`. `dicom.seg.from_dicomfile(df)` serializes the existing `DicomFile` into an owned copy because Python cannot move C++ unique ownership out of the object.

```python
raw = seg.to_array(0)  # dtype uint8
fraction = raw.astype("float32") / seg.maximum_fractional_value
```

`FrameOfReferenceUID` is the primary spatial compatibility key for overlay decisions. `SourceImageSequence` is provenance metadata, not a mandatory display target list.
