# Python API Overview

`dicomsdl` is a thin pybind11 wrapper. It loads a native extension at runtime, so docs build with a mock import; install the wheel to run the examples.

## Import
```python
import dicomsdl as dicom
```

## Key objects and functions
- `keyword_to_tag_vr(keyword: str) -> (Tag, VR)`: resolve a keyword to `(Tag, VR)`.
- `tag_to_keyword(tag: Tag | str) -> str`: resolve a tag to keyword.
- `read_file(path: str) -> DataSet`: load a dataset from file.
- `read_bytes(data: bytes, name: str = "inline") -> DataSet`: load from an in-memory buffer.
- `DataSet`: container of DICOM elements; supports `__iter__`, `__len__`, `add_dataelement`, `get_dataelement`, `__getitem__`, attribute access.
- `VR`: DICOM VR enum; constants like `VR.AE`, `VR.UI`, string via `str(vr)` or `vr.str()`.

## DataSet usage
```python
f = dicom.read_file("sample.dcm")
print(f.path)
print(len(f))

for elem in f:
    print(elem.tag, elem.vr, elem.length)

# Add an element (offset/length refer to original file offsets)
f.add_dataelement(tag=dicom.keyword_to_tag_vr("PatientName")[0], vr=dicom.VR.PN, offset=0, length=0)
```

## Error handling
- Invalid keyword/tag strings raise `ValueError`.
- Parse errors are surfaced as `RuntimeError`.

## Performance notes
- Keyword/tag lookups are constant-time (perfect hash).
- On large files, prefer targeted element access over full iteration in Python hot loops.
