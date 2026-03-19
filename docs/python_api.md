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
- `generate_uid() -> str`: create a new UID under DICOMSDL prefix.
- `append_uid(base_uid: str, component: int) -> str`: append one UID component with fallback policy.
- `DicomFile`: owns root dataset/session state; use `.dataset` for explicit root access.
- `DataSet`: container of DICOM elements; supports `__iter__`, `size()`, `add_dataelement`, `ensure_dataelement`, `get_dataelement`, `get_value`, `set_value`, `__getitem__`, attribute access.
- `VR`: DICOM VR enum; constants like `VR.AE`, `VR.UI`, string via `str(vr)` or `vr.str()`.

## DataSet usage
```python
df = dicom.read_file("sample.dcm")
print(df.path)
print(len(df))

for elem in df:
    print(elem.tag, elem.vr, elem.length)

# Add an element
df.add_dataelement(tag=dicom.keyword_to_tag_vr("PatientName")[0], vr=dicom.VR.PN)

# Hide OFFSET column when needed
print(df.dump(include_offset=False))

# If you need explicit root dataset object:
ds = df.dataset

rows_elem = ds["Rows"]
print(rows_elem.tag, rows_elem.vr, rows_elem.length, rows_elem.value)
rows_fast = ds.get_value("Rows")
rows_elem.value = rows_fast
ensured = ds.ensure_dataelement("Rows")
assert ensured.vr == dicom.VR.US
assert ds.set_value("Rows", rows_fast)
assert ds.set_value(0x00090030, dicom.VR.US, 16)
leaf = ds.ensure_dataelement(
    "ReferencedStudySequence.0.ReferencedSOPInstanceUID", dicom.VR.UI
)
assert ds.set_value("ReferencedStudySequence.0.ReferencedSOPInstanceUID", "1.2.3")
```

Partial-load note:

- `get_value(...)` does not implicitly continue loading unread tail elements.
- `set_value(...)` also does not implicitly continue loading unread tail elements.
- `add_dataelement(...)`, `ensure_dataelement(...)`, and `set_value(...)` accept dotted tag-path
  strings for nested sequence traversal and create intermediate sequence items as needed.
- direct `add_dataelement(...)` / `ensure_dataelement(...)` calls for tags beyond the current
  load frontier raise instead of mutating unread tail data.

Runnable Python examples:

- `examples/python/dataset_access_example.py`
- `examples/python/dump_dataset_example.py`

For the full user guide covering `DataSet`, `DicomFile`, `DataElement`,
recommended access patterns, and read/write examples, see
[Python DataSet Access Guide](python_dataset_access_design.md).

## Raw value bytes (no copy)

`DataElement.value_span()` returns a read-only `memoryview` over raw value bytes.

```python
ds = dicom.read_file("sample.dcm").dataset
elem = ds.get_dataelement("PixelData")
if elem:
    raw = elem.value_span()
    print(raw.nbytes, list(raw[:8]))
```

## Error handling
- Invalid keyword/tag strings raise `ValueError`.
- Parse errors are surfaced as `RuntimeError`.
- `decode_into` and `to_array` raise `ValueError` for invalid frame/buffer/layout
  requests, and `RuntimeError` when the native decode path fails after validation.

## Pixel decode safety warning
- If you mutate pixel-affecting fields (for example: transfer syntax, rows/cols,
  samples-per-pixel, bits allocated, pixel representation, planar configuration,
  number of frames, or pixel data elements), treat any previous decode layout
  assumptions as invalid.
- Re-fetch metadata and allocate a fresh output buffer before re-running
  `decode_into`.

## Performance notes
- Keyword/tag lookups are constant-time (perfect hash).
- On large files, prefer targeted element access over full iteration in Python hot loops.

## Pixel transform metadata

Frame-aware metadata resolution for:

- `DicomFile.rescale_transform_for_frame(frame_index)`
- `DicomFile.window_transform_for_frame(frame_index)`
- `DicomFile.voi_lut_for_frame(frame_index)`
- `DicomFile.modality_lut_for_frame(frame_index)`

is documented in [Pixel Transform Metadata Resolution](pixel_transform_metadata.md).

## Related docs
- Python DataSet Access Guide: [Python DataSet Access Guide](python_dataset_access_design.md)
- UID generation and append details: [Generating UID](generating_uid.md)
- Pixel encode constraints and codec-specific limits: [Pixel Encode Constraints](pixel_encode_constraints.md)
