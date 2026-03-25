# Troubleshooting

Use this page when the first build, read, decode, or write attempt fails and you need the quickest way to the likely cause.

## Common failure patterns

- wheel build fails before compilation:
  check Python, `pip`, `cmake`, compiler toolchain, and the active virtual environment
- later-tag mutation raises on a partially loaded file:
  load more of the file first, or avoid mutating data elements that have not been parsed yet
- `decode_into()` reports an array shape, dtype, or buffer-size mismatch:
  re-check rows, cols, samples per pixel, frame count, and output itemsize
- charset rewrite fails or replacement occurs:
  review the declared target charset and encode error policy
- tag/path lookup does not resolve:
  confirm the keyword spelling or dotted path form

## Charset declaration repair

Use this path only when the stored text bytes are already correct, but `(0008,0005) Specific Character Set` is missing or wrong. In that case `to_utf8_string()` or `to_person_name()` may fail even though the underlying bytes are fine.

Do not use this path as a normal transcode workflow. If the text needs to be rewritten into a different charset, use `set_specific_charset()` instead.

**C++**

```cpp
#include <dicom.h>
#include <iostream>

using namespace dicom::literals;

dicom::DicomFile file;
auto& study = file.add_dataelement("StudyDescription"_tag, dicom::VR::LO);

// These bytes are already UTF-8, but the dataset forgot to declare that fact.
study.from_string_view("심장 MRI");

if (!study.to_utf8_string()) {
    std::cout << "decode failed before declaration repair\n";
}

// Repair only the declaration. Existing bytes stay untouched.
file.set_declared_specific_charset(dicom::SpecificCharacterSet::ISO_IR_192);

if (auto utf8 = study.to_utf8_string()) {
    std::cout << *utf8 << '\n';
}
```

**Python**

```python
import dicomsdl as dicom

df = dicom.DicomFile()
study = df.dataset.add_dataelement(dicom.Tag("StudyDescription"), dicom.VR.LO)

# These bytes are already UTF-8, but the dataset forgot to declare that fact.
study.from_string_view("심장 MRI")

print(study.to_utf8_string())

# Repair only the declaration. Existing bytes stay untouched.
df.set_declared_specific_charset("ISO_IR 192")

print(study.to_utf8_string())
```

## Where to look next

- read/decode failures: [Error Handling](error_handling.md)
- charset text and PN overview: [Charset and Person Name](charset_and_person_name.md)
- nested path issues: [Sequence and Paths](sequence_and_paths.md)
- pixel encode issues: [Pixel Encode Constraints](../reference/pixel_encode_constraints.md)
- exact failure categories: [Error Model](../reference/error_model.md)
