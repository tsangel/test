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
- Charset updates: prefer `set_declared_specific_charset()` / `set_specific_charset()` instead of directly editing `(0008,0005)` as a raw element. Effective charset caches are synchronized by the charset setter APIs.
- Pixel decode safety: if pixel-affecting metadata changes (transfer syntax,
  rows/cols, samples, bits, pixel representation, planar configuration, frame
  count, pixel data tags), do not reuse old decode layout assumptions; re-query
  metadata and reallocate output buffers before `decode_into`.

### DicomFile
- Session wrapper that owns the root `DataSet`.
- Error status after read: `has_error` (bool), `error_message` (`str | None`).
- `set_declared_specific_charset()` updates only `(0008,0005) Specific Character Set`.
- `set_specific_charset()` transcodes text VR values and updates `(0008,0005)` together across
  the selected dataset subtree, including nested sequence-item datasets.
  Supports `errors='strict'`, `errors='replace_qmark'`, and
  `errors='replace_unicode_escape'`.
  With `return_replaced=True`, it returns whether replacement occurred.
- `write_bytes()` and `write_file()` write the dataset's current raw byte values as-is.

### DataElement
- Provides `tag`, `vr`, `length`, `offset`, and helpers to coerce values (`to_long`, `to_double`, `to_string_view`, `to_utf8_string`, `to_utf8_strings`, etc.).
- `from_utf8_view()` / `from_utf8_views()` encode immediately using the current dataset charset declaration.
  Supports `errors='strict'`, `errors='replace_qmark'`, and
  `errors='replace_unicode_escape'`.
  With `return_replaced=True`, they return `(ok, replaced)`.
- `to_utf8_string()` / `to_utf8_strings()` decode text using `(0008,0005)` and return owned Python strings.
  Supports `errors='strict'`, `errors='replace_fffd'`, and
  `errors='replace_hex_escape'`.
  With `return_replaced=True`, they return `(value_or_none, replaced)`.
- `to_string_view()` remains the raw trimmed-string helper for already encoded DICOM bytes.
- `to_string_views()` remains a raw helper, but returns `None` for declared multibyte text charsets
  such as ISO 2022 JIS, GBK, and GB18030 because raw-byte splitting on `\` is not safe before
  charset decode.
- Raw-byte helper: `value_span()` returns a read-only `memoryview` (no copy).
- Truthiness: `bool(elem)` is `False` for missing lookups (`VR.None`), otherwise `True`.
- Presence helpers: `elem.is_present()` / `elem.is_missing()` use the same rule as `bool(elem)`.
- Sequence helpers: `sequence`, `as_sequence`, `pixel_sequence`, `as_pixel_sequence`.

### Tag
- Construct from `(group, element)`, packed int, or keyword.
- Properties: `group`, `element`, `value`; `is_private`; `__str__` yields `(gggg,eeee)`.

### VR
- Enum-like VR wrapper; string conversion via `str(vr)` or `vr.str()`.
- Classification helpers include `is_string()`, `is_binary()`, `is_sequence()`,
  `is_pixel_sequence()`, `uses_specific_character_set()`, and
  `allows_multiple_text_values()`.

### Uid
- Construct from keyword or dotted string; throws on unknown values.

## Usage patterns
- Preferred import: `import dicomsdl as dicom`
- Load from file: `df = dicom.read_file('sample.dcm'); ds = df.dataset`
- Quick lookup: `tag, vr = dicom.keyword_to_tag_vr('PatientName')`
- Iteration: `for elem in ds: ...`
- Sequence traversal: `ds['RadiopharmaceuticalInformationSequence.0.RadionuclideTotalDose']`
