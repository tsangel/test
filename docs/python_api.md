# Python API Overview

`dicomsdl` is a thin nanobind wrapper. It loads a native extension at runtime, so docs build with a mock import; install the wheel to run the examples.

## Import
```python
import dicomsdl as dicom
```

## Key objects and functions
- `keyword_to_tag_vr(keyword: str) -> (Tag, VR)`: resolve a keyword to `(Tag, VR)`.
- `tag_to_keyword(tag: Tag | str) -> str`: resolve a tag to keyword.
- `read_file(path: str) -> DicomFile`: load a DICOM file/session from disk.
- `read_bytes(data: bytes, name: str = "inline") -> DicomFile`: load from an in-memory buffer.
- `DicomFile`: owns root dataset/session state; use `.dataset` for explicit root access.
- `DataSet`: container of DICOM elements; supports `__iter__`, `size()`, `add_dataelement`, `get_dataelement`, `__getitem__`, attribute access.
- `VR`: DICOM VR enum; constants like `VR.AE`, `VR.UI`, string via `str(vr)` or `vr.str()`.

## DataSet usage
```python
df = dicom.read_file("sample.dcm")
print(df.path)
print(len(df))

for elem in df:
    print(elem.tag, elem.vr, elem.length)

# Add an element (offset/length refer to original file offsets)
df.add_dataelement(tag=dicom.keyword_to_tag_vr("PatientName")[0], vr=dicom.VR.PN, offset=0, length=0)

# Hide OFFSET column when needed
print(df.dump(include_offset=False))

# If you need explicit root dataset object:
ds = df.dataset
```

## Error handling
- Invalid keyword/tag strings raise `ValueError`.
- Parse errors are surfaced as `RuntimeError`.

## Pixel decode safety warning
- Pixel decode paths may cache pixel metadata internally.
- If you mutate pixel-affecting fields (for example: transfer syntax, rows/cols,
  samples-per-pixel, bits allocated, pixel representation, planar configuration,
  number of frames, or pixel data elements), treat any previous decode layout
  assumptions as invalid.
- Re-fetch metadata and allocate a fresh output buffer before re-running
  `decode_into`.

## Performance notes
- Keyword/tag lookups are constant-time (perfect hash).
- On large files, prefer targeted element access over full iteration in Python hot loops.
