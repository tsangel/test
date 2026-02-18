# Python API Reference

This page collects the primary Python entry points exposed by `dicomsdl`'s nanobind module. Docstrings are defined in `bindings/python/dicom_module.cpp` and rendered here for convenience.

## Module-level functions

### read_file(path, load_until=None, keep_on_error=None)
Read a DICOM file from disk and return a `DataSet`. Parsing is eager up to `load_until` (default: entire file). When `keep_on_error=True`, partially read data is kept instead of throwing.

### read_bytes(data, name='<memory>', load_until=None, keep_on_error=None, copy=True)
Read a `DataSet` from a bytes-like object. Parsing is eager up to `load_until`.

Warning: when `copy=False`, the source buffer must remain alive for as long as the returned `DataSet`; the binding keeps a Python reference, but mutating or freeing the underlying memory can corrupt the dataset.

## Classes

### DataSet
- Iterable container of `DataElement` objects in tag order.
- Indexing: `ds[tag|packed_int|tag_str]` returns the element's value or `None` if missing.
- Attribute sugar: `ds.PatientName` resolves keyword → tag and returns the value, raising `AttributeError` if missing.
- Methods: `add_dataelement`, `remove_dataelement`, `get_dataelement` (returns `NullElement` on miss), `dump_elements`, `path`.

### DataElement
- Provides `tag`, `vr`, `length`, `offset`, and helpers to coerce values (`to_long`, `to_double`, `to_string_view`, `to_utf8_view`, etc.).
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
- Load from file: `ds = dicom.read_file('sample.dcm')`
- Quick lookup: `tag, vr = dicom.keyword_to_tag_vr('PatientName')`
- Iteration: `for elem in ds: ...`
- Sequence traversal: `ds['RadiopharmaceuticalInformationSequence.0.RadionuclideTotalDose']`
