# Python DataSet Access Guide

This guide is the main user-facing reference for Python-side dataset and element access in `dicomsdl`.
It explains the mental model, the recommended APIs, and the most important read/write patterns.

## Mental model

`dicomsdl` exposes two related Python objects:

- `DicomFile`: file/session wrapper that owns the root dataset
- `DataSet`: container of DICOM `DataElement` objects

The binding uses a split model on purpose:

- attribute access is value-oriented: `ds.Rows`
- index access is element-oriented: `ds["Rows"]`

That keeps common reads short, while still making VR/length/tag metadata easy to inspect.

## Recommended API surface

| API | Returns | Missing behavior | Intended use |
| --- | --- | --- | --- |
| `ds.Rows` | typed value | `AttributeError` | ergonomic sugar for known/common tags |
| `"Rows" in ds` | `bool` | `False` | presence probe |
| `ds.get_value("Rows", default=None)` | typed value or `default` | returns `default` | optional value lookup |
| `ds["Rows"]` | `DataElement` | returns a falsey `NullElement` sentinel, no exception | element-oriented access aligned with C++ |
| `ds.get_dataelement("Rows")` | `DataElement` | returns a falsey `NullElement` sentinel, no exception | optional metadata lookup with chaining |
| `ds.ensure_dataelement("Rows", vr=None)` | `DataElement` | returns existing or inserts zero-length element | chaining-friendly ensure/create API |
| `ds.set_value("Rows", 512)` | `bool` | writes or zero-lengths | one-shot typed assignment |
| `ds.set_value(0x00090030, dicom.VR.US, 16)` | `bool` | creates or overrides by explicit VR | private or ambiguous tags |

## Reading datasets

### read_file(path, load_until=None, keep_on_error=None)

Read a DICOM file from disk and return a `DicomFile`.
Parsing is eager up to `load_until` (default: entire file).
When `keep_on_error=True`, partially read data is kept instead of throwing; check `DicomFile.has_error` and `DicomFile.error_message` after read.

### read_bytes(data, name="<memory>", load_until=None, keep_on_error=None, copy=True)

Read a `DicomFile` from a bytes-like object.
Parsing is eager up to `load_until`.

Warning: when `copy=False`, the source buffer must remain alive for as long as the returned `DicomFile`; the binding keeps a Python reference, but mutating or freeing the underlying memory can corrupt the dataset.

## DataSet and DicomFile

`DicomFile` forwards unknown attributes and methods to its root dataset, so these patterns are equivalent:

```python
df = dicom.read_file("sample.dcm")

rows1 = df.Rows
rows2 = df.dataset.Rows

elem1 = df["Rows"]
elem2 = df.dataset["Rows"]
```

### Attribute access returns the typed value

```python
rows = ds.Rows
patient_name = ds.PatientName
```

Use this when you expect the element to be present and you want the actual value, not metadata.

### Index access returns a DataElement

```python
elem = ds["Rows"]
if elem:
    print(elem.tag, elem.vr, elem.length, elem.value)
```

Missing lookups return a falsey sentinel rather than raising:

```python
missing = ds["NotARealKeyword"]
assert not missing
assert missing.value is None
```

### Presence checks

Use `in` when you only need to know whether an element exists:

```python
if "Rows" in ds:
    rows = ds["Rows"].value

if dicom.Tag("PatientName") in df:
    print(df["PatientName"].value)
```

Accepted key types are:

- `str` keyword or tag-path string
- `Tag`
- packed `int` such as `0x00280010`

Malformed string keys return `False`.

### Explicit metadata lookup

`get_dataelement(...)` is the explicit named lookup API:

```python
elem = ds.get_dataelement("PatientName")
if elem:
    print(elem.vr, elem.length, elem.value)
```

It uses the same missing-element sentinel behavior as `ds[...]`.

### Ensure-or-create lookup

`ensure_dataelement(...)` is the chaining-friendly "make sure this element exists" API:

```python
rows = ds.ensure_dataelement("Rows")
private_value = ds.ensure_dataelement(0x00090030, dicom.VR.US)
```

Rules:

- if the element already exists and `vr` is omitted or `None`, the existing element is returned unchanged
- if the element already exists and `vr` is explicit but different, the existing element is still returned unchanged
- if the element is missing, a new zero-length element is inserted
- unlike `add_dataelement(...)`, this API does not replace an existing element

### Iteration and size

```python
for elem in ds:
    print(elem.tag, elem.vr, elem.length)

print(ds.size())
print(len(df))
```

### dump()

`dump()` returns a human-readable dataset dump:

```python
print(df.dump())
print(ds.dump(include_offset=False))
```

## Reading values

### Fast path: get_value()

Use `get_value()` for optional one-shot value reads:

```python
rows = ds.get_value("Rows")
window_center = ds.get_value("WindowCenter", default=None)
```

This is the shortest non-raising value path when you do not need the `DataElement` object.

`default` is used only for missing elements. A present zero-length element still returns a typed empty value:

```python
assert ds.get_value("PatientName", default="DEFAULT") == "DEFAULT"  # missing
assert ds.get_value("Rows", default="DEFAULT") == []                # present, zero-length US
```

### Chained path: ds["Rows"].value

Use this when you need metadata and value together:

```python
rows_elem = ds["Rows"]
if rows_elem:
    print(rows_elem.vr)
    print(rows_elem.value)
```

### Value types returned by DataElement.value / get_value()

- `SQ` / `PX` -> `Sequence` / `PixelSequence`
- numeric-like VRs (`IS`, `DS`, `AT`, `FL`, `FD`, `SS`, `US`, `SL`, `UL`, `SV`, `UV`) -> `int`, `float`, `Tag`, or `list[...]`
- `PN` -> `PersonName` or `list[PersonName]` when parsing succeeds
- charset-aware text VRs -> UTF-8 `str` or `list[str]`
- charset decode or `PN` parse failure -> raw `bytes`
- binary VRs -> read-only `memoryview`

For zero-length present elements:

- numeric-like VRs return `[]`
- text VRs return `""`
- `PN` returns an empty `PersonName`
- binary VRs return an empty read-only `memoryview`
- `SQ` / `PX` return empty container objects

This matches the underlying C++ vector accessors: zero-length numeric-like values are treated as parseable empty vectors, not as missing values.

### Zero-length return matrix

The most important rule is that a zero-length element is still *present*. It does not use the `default` argument and it does not behave like a missing lookup.

In particular, some string VRs can normally have `VM > 1`, but a zero-length value still reads back as an empty scalar-style value because `vm()` is `0`, not `> 1`.

| VR family | Non-empty `VM == 1` | Non-empty `VM > 1` | Zero-length present |
| --- | --- | --- | --- |
| `AE`, `AS`, `CS`, `DA`, `DT`, `TM`, `UI`, `UR` | `str` | `list[str]` | `""` |
| `LO`, `LT`, `SH`, `ST`, `UC`, `UT` | `str` | `list[str]` for multi-value-capable VRs; otherwise `str` | `""` |
| `PN` | `PersonName` | `list[PersonName]` when parsing succeeds | empty `PersonName` |
| `IS`, `DS` | `int` / `float` | `list[int]` / `list[float]` | `[]` |
| `AT` | `Tag` | `list[Tag]` | `[]` |
| `FL`, `FD`, `SS`, `US`, `SL`, `UL`, `SV`, `UV` | `int` / `float` | `list[int]` / `list[float]` | `[]` |
| `OB`, `OD`, `OF`, `OL`, `OW`, `OV`, `UN` | `memoryview` | not used as Python list values | empty `memoryview` |
| `SQ`, `PX` | sequence object | sequence-like container | empty container object |

Examples:

```python
assert ds.get_value("ImageType") == ["ORIGINAL", "PRIMARY"]
assert ds.get_value("ImageType", default="DEFAULT") == ""   # present, zero-length CS

assert ds.get_value("PatientName", default="DEFAULT") == "DEFAULT"  # missing
assert str(ds["PatientName"].value) == ""                           # present, zero-length PN

assert ds.get_value("Rows", default="DEFAULT") == []               # present, zero-length US
assert ds.get_value("WindowCenter", default="DEFAULT") == []       # present, zero-length DS
```

For direct vector accessors, zero-length values also return empty containers rather than `None`:

```python
assert ds["Rows"].to_longlong_vector() == []
assert ds["WindowCenter"].to_double_vector() == []
assert ds["FrameIncrementPointer"].to_tag_vector() == []
```

At the C++ layer the same contract now applies to the vector accessors:

```cpp
auto rows = dataset["Rows"_tag].to_longlong_vector();   // engaged optional, empty vector when zero-length
auto wc = dataset["WindowCenter"_tag].to_double_vector();
auto at = dataset["FrameIncrementPointer"_tag].to_tag_vector();
```

### Distinguishing zero-length from missing

In `dicomsdl`, `missing` and `zero-length` are different element states and should be tested at the `DataElement` level, not by looking only at `elem.value`.

Use this rule:

```python
elem = ds["PatientName"]

if not elem:
    # missing lookup
elif elem.length == 0:
    # present element with zero-length value
else:
    # present element with non-empty value
```

Practical differences:

- missing element
  - `bool(elem) == False`
  - `elem.is_missing() == True`
  - `elem.vr == dicom.VR.None`
  - `elem.value is None`
- zero-length present element
  - `bool(elem) == True`
  - `elem.is_missing() == False`
  - `elem.vr != dicom.VR.None`
  - `elem.length == 0`

This distinction matters for DICOM attributes where "present but empty" is semantically different from "not present".

## Writing values

### Update an existing element through the returned DataElement

```python
ds["Rows"].value = 512
```

This is the natural path once you already have the element object.

### One-shot assignment with set_value()

```python
assert ds.set_value("Rows", 512)
assert ds.set_value("StudyDescription", "Example")
assert ds.set_value("Rows", None)   # present, zero-length US
```

This is the best path when you want create/update by key in one call.

Failure model:

- on success, the requested value is written
- on failure, `set_value()` returns `False`
- the `DataSet` / `DicomFile` remains usable
- the destination element state is unspecified and should not be relied on

If you need rollback semantics, keep the previous value yourself and restore it explicitly.

### Creating zero-length values vs removing elements

`None` means zero-length present value:

```python
assert ds.set_value("PatientName", None)   # present, zero-length PN
assert ds.set_value("Rows", None)          # present, zero-length US
```

`None` is just shorthand for the appropriate zero-length representation of the resolved VR.
You can also spell the same intent with an explicit empty payload:

```python
ds.add_dataelement(dicom.Tag("PatientName"), dicom.VR.PN)   # present, zero-length

assert ds.set_value("PatientName", "")      # zero-length text element
assert ds.set_value("Rows", [])             # zero-length numeric VM-based element
assert ds.set_value(0x00111001, dicom.VR.OB, b"")  # zero-length binary element
```

The same rule applies through an existing element:

```python
ds["PatientName"].value = None
ds["PatientName"].value = ""
ds["Rows"].value = None
ds["Rows"].value = []
```

Recommended interpretation:

- `None` -> keep or create a present zero-length element
- empty payload (`""`, `[]`, `b""`) -> keep the element present with `length == 0`

Use `remove_dataelement()` for deletion:

```python
ds.remove_dataelement("PatientName")
ds.remove_dataelement(0x00280010)
ds.remove_dataelement(dicom.Tag("Rows"))
```

### Explicit VR assignment for private or ambiguous tags

```python
assert ds.set_value(0x00090030, dicom.VR.US, 16)
```

This overload is useful when the tag is private or when you want to override an existing non-sequence element VR before assigning a value.

Rules:

- if the element is missing, the supplied VR is used to create it
- if the element exists and already has that VR, the value is updated in place
- if the element exists with a different non-`SQ`/non-`PX` VR, the binding may replace that VR and then assign the value
- `VR.None`, `SQ`, and `PX` are not valid override targets for this overload

This overload does not provide rollback semantics. If it returns `False`, the dataset
remains valid but the destination element state is unspecified.

### Indexed assignment is intentionally unsupported

```python
ds["Rows"] = 512  # not supported
```

Once `ds[...]` returns a `DataElement`, writes should go through either:

- `ds["Rows"].value = 512`
- `ds.set_value("Rows", 512)`

### Attribute assignment sugar

```python
ds.Rows = 512
df.PatientName = pn
```

Attribute assignment remains value-oriented sugar for standard keyword-based updates.

## DataElement

`DataElement` is the main metadata-bearing object.

### Core properties

- `elem.value`
- `elem.tag`
- `elem.vr`
- `elem.length`
- `elem.offset`
- `elem.vm`

These are properties, not method calls:

```python
elem = ds["Rows"]
print(elem.tag)
print(elem.vr)
print(elem.length)
```

### Truthiness and missing sentinel

```python
elem = ds["PatientName"]
if elem:
    ...

missing = ds["NotARealKeyword"]
assert not missing
assert missing.is_missing()
```

`bool(elem)` matches `elem.is_present()`.

For zero-length present elements, `bool(elem)` is still `True`; use `elem.length == 0` to detect them.

### Value helpers

- `elem.get_value()` mirrors `elem.value`
- `elem.set_value(value)` mirrors the `value` setter and returns `True`/`False`
- failed `elem.set_value(value)` leaves the owning dataset valid, but the target element state is unspecified
- coercion helpers include `to_long()`, `to_double()`, `to_string_view()`, `to_utf8_string()`, `to_utf8_strings()`, `to_person_name()`, and related vector variants

### Raw bytes

`value_span()` returns a read-only `memoryview` without copying:

```python
raw = ds.get_dataelement("PixelData").value_span()
print(raw.nbytes)
```

### Sequence and pixel sequence helpers

- `elem.sequence`
- `elem.pixel_sequence`
- `elem.is_sequence`
- `elem.is_pixel_sequence`

## Text VRs, charsets, and PersonName

For text VRs and `PN`, prefer the charset-aware helpers when you care about correct decoding and encoding.

### Charset-aware reads

- `to_utf8_string()`
- `to_utf8_strings()`
- `to_person_name()`
- `to_person_names()`

### Charset-aware writes

- `from_utf8_view()`
- `from_utf8_views()`
- `from_person_name()`
- `from_person_names()`

`set_declared_specific_charset()` and `set_specific_charset()` are preferred over directly mutating `(0008,0005) Specific Character Set` as a raw element.

### PersonNameGroup

Represents one `PN` component group with up to 5 components.

Properties:

- `components`
- `family_name`
- `given_name`
- `middle_name`
- `name_prefix`
- `name_suffix`

Methods:

- `component(index)`
- `empty()`
- `to_dicom_string()`

### PersonName

Represents a parsed `PN` value with up to 3 component groups:

- `alphabetic`
- `ideographic`
- `phonetic`

Methods:

- `empty()`
- `to_dicom_string()`

Example:

```python
pn = dicom.PersonName(
    alphabetic=("Yamada", "Tarou"),
    ideographic=("山田", "太郎"),
    phonetic=("やまだ", "たろう"),
)

assert pn.to_dicom_string() == "Yamada^Tarou=山田^太郎=やまだ^たろう"
```

## Supporting types

### Tag

Construct from:

- `(group, element)`
- packed `int`
- keyword

Important properties:

- `group`
- `element`
- `value`
- `is_private`

`str(tag)` renders as `(gggg,eeee)`.

### VR

Enum-like VR wrapper with constants such as `VR.AE`, `VR.US`, and `VR.UI`.

Useful helpers:

- `str(vr)` / `vr.str()`
- `is_string()`
- `is_binary()`
- `is_sequence()`
- `is_pixel_sequence()`
- `uses_specific_character_set()`
- `allows_multiple_text_values()`

### Uid

Construct from a keyword or dotted string.
Unknown values raise.

## DicomFile-specific notes

- `df.dataset` gives the explicit root dataset object
- `df.has_error` and `df.error_message` expose read status after partial/error-tolerant parsing
- `write_bytes()` and `write_file()` write the dataset's current raw byte values as-is

## Error handling

- invalid keyword/tag strings raise `ValueError` on strict lookup paths
- parse failures surface as `RuntimeError`
- `decode_into()` and `to_array()` raise `ValueError` for invalid frame/buffer/layout requests, and `RuntimeError` when the native decode path fails after validation

## Pixel decode safety

If you mutate pixel-affecting metadata such as transfer syntax, rows/cols, samples per pixel, bits allocated, pixel representation, planar configuration, number of frames, or pixel data elements, do not reuse old decode layout assumptions.
Re-fetch metadata and allocate a fresh output buffer before calling `decode_into()` again.

## Migration from the older Python API

The main behavior changes are:

1. `DataSet.__getitem__` returns `DataElement`, not the typed value.
2. `DataElement.value` is the primary chained access path.
3. `DataSet.get_value(...)` is the one-shot value fast path.
4. `DataSet.set_value(...)` is the one-shot value write path.
5. `ds["Rows"] = value` is no longer supported.

## Related docs

- High-level overview: [Python API Overview](python_api.md)
- UID generation details: [Generating UID](generating_uid.md)
- Pixel encode limits: [Pixel Encode Constraints](pixel_encode_constraints.md)
