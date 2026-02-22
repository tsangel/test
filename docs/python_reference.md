# Python API Reference

This page collects the primary Python entry points exposed by `dicomsdl`'s nanobind module. Docstrings are defined in `bindings/python/dicom_module.cpp` and rendered here for convenience.

## Module-level functions

### read_file(path, load_until=None, keep_on_error=None)
Read a DICOM file from disk and return a `DicomFile`. Parsing is eager up to `load_until` (default: entire file). When `keep_on_error=True`, partially read data is kept instead of throwing; check `DicomFile.has_error` and `DicomFile.error_message` after read.

### read_bytes(data, name='<memory>', load_until=None, keep_on_error=None, copy=True)
Read a `DicomFile` from a bytes-like object. Parsing is eager up to `load_until`.

Warning: when `copy=False`, the source buffer must remain alive for as long as the returned `DicomFile`; the binding keeps a Python reference, but mutating or freeing the underlying memory can corrupt the dataset.

## Classes

### DataSet
- Iterable container of `DataElement` objects in tag order.
- Indexing: `ds[tag|packed_int|tag_str]` returns the element's value or `None` if missing.
- Attribute sugar: `ds.PatientName` resolves keyword → tag and returns the value, raising `AttributeError` if missing.
- Methods: `add_dataelement`, `remove_dataelement`, `get_dataelement` (returns a falsey `DataElement` with `VR.None` on miss), `dump_elements`, `path`.
- Pixel decode safety: if pixel-affecting metadata changes (transfer syntax,
  rows/cols, samples, bits, pixel representation, planar configuration, frame
  count, pixel data tags), do not reuse old decode layout assumptions; re-query
  metadata and reallocate output buffers before `decode_into`.

### DicomFile
- Session wrapper that owns the root `DataSet`.
- Error status after read: `has_error` (bool), `error_message` (`str | None`).

### DataElement
- Provides `tag`, `vr`, `length`, `offset`, and helpers to coerce values (`to_long`, `to_double`, `to_string_view`, `to_utf8_view`, etc.).
- Truthiness: `bool(elem)` is `False` for missing lookups (`VR.None`), otherwise `True`.
- Presence helpers: `elem.is_present()` / `elem.is_missing()` use the same rule as `bool(elem)`.
- Sequence helpers: `sequence`, `as_sequence`, `pixel_sequence`, `as_pixel_sequence`.

### Tag
- Construct from `(group, element)`, packed int, or keyword.
- Properties: `group`, `element`, `value`; `is_private`; `__str__` yields `(gggg,eeee)`.

### VR
- Enum-like VR wrapper; string conversion via `str(vr)` or `vr.str()`.

### Uid
- Construct from keyword or dotted string; throws on unknown values.

## Usage patterns
- Preferred import: `import dicomsdl as dicom`
- Load from file: `df = dicom.read_file('sample.dcm'); ds = df.dataset`
- Quick lookup: `tag, vr = dicom.keyword_to_tag_vr('PatientName')`
- Iteration: `for elem in ds: ...`
- Sequence traversal: `ds['RadiopharmaceuticalInformationSequence.0.RadionuclideTotalDose']`
