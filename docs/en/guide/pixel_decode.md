# Pixel Decode

Use `to_array()` for the simplest decoded output path and `decode_into()` when you want to reuse a preallocated output buffer.

## Relevant DICOM standard sections

- The pixel attributes that control rows, columns, samples per pixel, photometric interpretation, and Pixel Data live in [DICOM PS3.3 Section C.7.6.3, Image Pixel Module](https://dicom.nema.org/medical/dicom/current/output/chtml/part03/sect_C.7.6.3.html).
- Native versus encapsulated Pixel Data encoding is defined in [DICOM PS3.5 Chapter 8, Encoding of Pixel, Overlay and Waveform Data](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/chapter_8.html) and [Section 8.2, Native or Encapsulated Format Encoding](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/sect_8.2.html).
- Encapsulated fragment/item layout and transfer syntax requirements are defined in [DICOM PS3.5 Section A.4, Transfer Syntaxes for Encapsulation of Encoded Pixel Data](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/sect_A.4.html).
- In file-based workflows, the Transfer Syntax UID comes from the file meta information described in [DICOM PS3.10 Chapter 7, DICOM File Format](https://dicom.nema.org/medical/dicom/current/output/chtml/part10/chapter_7.html).

## C++

```cpp
#include <dicom.h>

auto file = dicom::read_file("sample.dcm");
auto decoded = file->pixel_buffer(0);
```

## Python

```python
import dicomsdl as dicom
import numpy as np

df = dicom.read_file("sample.dcm")
arr = df.to_array(frame=0)
out = np.empty_like(arr)
df.decode_into(out, frame=0)
```

## Notes

- If you mutate pixel-affecting metadata such as transfer syntax, rows, columns, samples per pixel, bits allocated, pixel representation, planar configuration, number of frames, or pixel data elements, do not reuse old decode layout assumptions.
- Re-fetch metadata and allocate a fresh output buffer before calling `decode_into()` again.
- `decode_into()` is the right path for benchmark or hot-loop reuse scenarios.
- `to_array()` is the right path for the quickest first success.

## Related docs

- [Quickstart](quickstart.md)
- [Pixel Encode](pixel_encode.md)
- [Pixel Transform Metadata Resolution](../reference/pixel_transform_metadata.md)
