# Pixel Encode

Use `set_pixel_data()` to replace Pixel Data from a contiguous numeric buffer and `set_transfer_syntax()` when you want dicomsdl to transcode using an encoder context.

## Relevant DICOM standard sections

- The pixel metadata that must stay consistent with encoded data is defined in [DICOM PS3.3 Section C.7.6.3, Image Pixel Module](https://dicom.nema.org/medical/dicom/current/output/chtml/part03/sect_C.7.6.3.html).
- Native versus encapsulated Pixel Data encoding and the codec-specific 8.2.x rules are defined in [DICOM PS3.5 Chapter 8, Encoding of Pixel, Overlay and Waveform Data](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/chapter_8.html) and [Section 8.2, Native or Encapsulated Format Encoding](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/sect_8.2.html).
- Encapsulated transfer syntax and fragment rules are defined in [DICOM PS3.5 Section A.4, Transfer Syntaxes for Encapsulation of Encoded Pixel Data](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/sect_A.4.html).
- In file-based encode/transcode workflows, the resulting Transfer Syntax UID is carried in the file meta information defined by [DICOM PS3.10 Chapter 7, DICOM File Format](https://dicom.nema.org/medical/dicom/current/output/chtml/part10/chapter_7.html).

## C++

```cpp
#include <dicom.h>

auto file = dicom::read_file("sample.dcm");
// Prepare a pixel::PixelSource describing your contiguous input buffer and layout.
// file->set_pixel_data(dicom::uid::WellKnown::JPEG2000Lossless, source);
```

## Python

```python
import dicomsdl as dicom

df = dicom.read_file("sample.dcm")
# arr must be a C-contiguous numeric buffer with a supported shape/dtype.
df.set_pixel_data("JPEG2000Lossless", arr)
df.write_file("encoded.dcm")
```

## Notes

- Input shape, dtype, and codec options must satisfy the current encoder contract.
- dicomsdl updates pixel metadata and transfer syntax state as part of the encode path.
- When you need the exact per-codec rules, use the reference page instead of guessing from a short example.

## Related docs

- [Pixel Decode](pixel_decode.md)
- [Pixel Encode Constraints](../reference/pixel_encode_constraints.md)
