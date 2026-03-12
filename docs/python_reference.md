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
- Assignment sugar: `ds.Rows = 512`, `ds["StudyDescription"] = "text"`, `df.PatientName = pn`.
- Binary VRs (`OB`, `OD`, `OF`, `OL`, `OW`, `OV`) also accept matching typed arrays
  via the Python buffer protocol, for example `array.array('H', ...)` for `OW`
  and `array.array('d', ...)` for `OD`.
- Setting a keyword/tag to `None` removes that element.
  For `PN`, this returns `PersonName` when parsing succeeds. For charset-aware text VRs, returned strings are UTF-8 decoded.
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
- `get_value()` is best-effort typed access.
  - `SQ` / `PX` -> `Sequence` / `PixelSequence`
  - numbers -> `int` / `float`
  - `PN` -> `PersonName` / `list[PersonName]` when parsing succeeds
  - charset-aware text -> UTF-8 `str` / `list[str]`
  - charset decode or PN parse failure -> raw `bytes`
  - binary VRs -> read-only `memoryview`
- Provides `tag`, `vr`, `length`, `offset`, and helpers to coerce values (`to_long`, `to_double`, `to_string_view`, `to_utf8_string`, `to_utf8_strings`, etc.).
- Structured PN helpers:
  - `to_person_name()` decodes a single-valued `PN` into a `PersonName`.
  - `to_person_names()` decodes all `PN` values into `list[PersonName]`.
  - `from_person_name()` / `from_person_names()` serialize structured `PN` data back into DICOM text.
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

### PersonNameGroup
- Represents one `PN` component group with up to 5 components.
- Constructor accepts 0 to 5 strings. Missing components are padded with `''`.
- `component(index)` is the neutral API. Component order is the standard DICOM order.
- The named aliases (`family_name`, `given_name`, `middle_name`, `name_prefix`, `name_suffix`) follow the human-use meaning from PS3.5 6.2. Veterinary `PN` uses different semantics for the first two components.
- Properties:
  - `components`
  - `family_name`
  - `given_name`
  - `middle_name`
  - `name_prefix`
  - `name_suffix`
- Methods:
  - `component(index)`
  - `empty()`
  - `to_dicom_string()`

Example:
```python
group = dicom.PersonNameGroup(("Yamada", "Tarou"))
assert group.components == ("Yamada", "Tarou", "", "", "")
assert group.to_dicom_string() == "Yamada^Tarou"
```

### PersonName
- Represents a parsed `PN` value with 3 optional component groups:
  - `alphabetic`
  - `ideographic`
  - `phonetic`
- Each group can be `None`, a `PersonNameGroup`, or a short sequence of strings passed to the constructor.
- Methods:
  - `empty()`
  - `to_dicom_string()`

Example:
```python
pn = dicom.PersonName(
    alphabetic=("Yamada", "Tarou"),
    ideographic=("??", "??"),
    phonetic=("???", "???"),
)

assert pn.to_dicom_string() == "Yamada^Tarou=??^??=???^???"
```

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
