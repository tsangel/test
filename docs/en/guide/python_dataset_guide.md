# Python DataSet Guide

`dicomsdl` is a thin nanobind wrapper. It loads a native extension at runtime, so the docs build with the extension import mocked out; install the wheel to run the examples.

This is the main user-facing guide for Python-side file, dataset, and element access in `dicomsdl`.
It covers module-level entry points, how dicomsdl objects map to DICOM, and the most important read/write patterns.

## Import

```python
import dicomsdl as dicom
```

## Module-level entry points

- `keyword_to_tag_vr(keyword: str) -> (Tag, VR)`: resolve a keyword to `(Tag, VR)`.
- `tag_to_keyword(tag: Tag | str) -> str`: resolve a tag to a keyword.
- `read_file(path) -> DicomFile`: load a DICOM file/session from disk.
- `read_bytes(data, name="inline") -> DicomFile`: load from an in-memory buffer.
- `generate_uid() -> str`: create a new UID under the DICOMSDL prefix.
- `append_uid(base_uid: str, component: int) -> str`: append one UID component with fallback policy.

## How dicomsdl maps to DICOM

`dicomsdl` exposes a small set of related Python objects:

- `DicomFile`: file/session wrapper that owns the root dataset
- `DataSet`: container of DICOM `DataElement` objects
- `DataElement`: one DICOM field, with tag / VR / length metadata and typed value access
- `Sequence`: nested item container for `SQ` values
- `PixelSequence`: frame / fragment container for encapsulated or compressed pixel data

For the object model and DICOM mapping, see [Core Objects](core_objects.md).

The binding uses a split model on purpose:

- attribute access is value-oriented: `ds.Rows`
- index access returns a `DataElement`: `ds["Rows"]`

That keeps common reads short, while still making VR/length/tag metadata easy to inspect.

### DicomFile and DataSet

Most data element access APIs are implemented on `DataSet`.
`DicomFile` owns the root `DataSet` and handles file-oriented work such as load, save, and transcode.
For convenience, `DicomFile` forwards root-dataset access, so `df.Rows`, `df["Rows"]`, `df.get_value(...)`, and `df.Rows = 512` all delegate to `df.dataset`.
If you bind `ds = df.dataset`, you are using the same dataset APIs directly, without forwarding.

These patterns are equivalent:

```python
df = dicom.read_file("sample.dcm")

rows1 = df.Rows
rows2 = df.dataset.Rows

elem1 = df["Rows"]
elem2 = df.dataset["Rows"]

df.Rows = 512
df.dataset.Rows = 512
```

## Recommended API surface

| API | Returns | Missing behavior | Intended use |
| --- | --- | --- | --- |
| `"Rows" in ds` | `bool` | `False` | presence probe |
| `ds.get_value("Rows", default=None)` | typed value or the supplied default value | returns the supplied default value | one-shot typed read where `None` or another default value means missing |
| `ds["Rows"]`, `ds.get_dataelement("Rows")` | `DataElement` | returns a `NullElement` that evaluates to `False`, no exception | `DataElement` access |
| `ds.ensure_loaded("Rows")` | `None` | raises on invalid key | explicitly continue a partial read up to a later tag such as `Rows` |
| `ds.ensure_dataelement("Rows", vr=None)` | `DataElement` | returns existing or inserts zero-length element | chaining-friendly ensure/create API |
| `ds.set_value("Rows", 512)` | `bool` | writes or zero-lengths | one-shot typed assignment |
| `ds.set_value(0x00090030, dicom.VR.US, 16)` | `bool` | creates or overrides by explicit VR | private or ambiguous tags |
| `ds.Rows` | typed value | `AttributeError` | development / interactive convenience access for known/common tags |
| `ds.Rows = 512` | `None` | raises on assignment failure | development / interactive convenience assignment for standard keyword updates |

## Ways to identify a Data Element in Python

| Form | Example | Use first when |
| --- | --- | --- |
| packed int | `0x00280010` | the tag already comes from numeric tables or external metadata |
| keyword or Tag string | `"Rows"`, `"(0028,0010)"` | most normal Python code |
| dotted tag-path string | `"RadiopharmaceuticalInformationSequence.0.RadionuclideTotalDose"` | you want a nested lookup or assignment in one step |
| `Tag` object | `dicom.Tag("Rows")`, `dicom.Tag(0x0028, 0x0010)` | you want an explicit reusable tag object |

### `0x00280010`

- Use this when the tag already comes from numeric constants, generated tables, or external metadata.
- Advantages: fastest direct path for a single tag, no string parse, works in single-tag APIs including `ensure_loaded(...)`.
- Tradeoffs: harder to read than keywords and cannot express nested paths.

### `"Rows"` or `"(0028,0010)"`

- Use this first in most normal Python code.
- Advantages: short, readable, works across common lookup/write APIs.
- Tradeoffs: small runtime keyword/Tag parse cost; nested access needs a dotted path string.

### Dotted tag-path strings

- Use this when you want a nested lookup or assignment in one step.
- Examples: `"RadiopharmaceuticalInformationSequence.0.RadionuclideTotalDose"`, `"00540016.0.00181074"`.
- Advantages: readable one-shot access into nested datasets.
- Tradeoffs: only APIs that support nested paths accept it; flat-only APIs such as `ensure_loaded(...)` and `remove_dataelement(...)` do not.

### `dicom.Tag(...)`

- Use this when you want an explicit tag object or want to reuse the same tag across calls.
- Advantages: explicit type, reusable, good at API boundaries.
- Tradeoffs: for one-off calls this adds Python-level `Tag` object construction; packed ints are more direct when you do not need reuse.

Practical recommendation:

- use `"Rows"` for usual keyword/Tag-string access in most Python code. This still has a small runtime keyword/Tag parse cost, but dicomsdl uses an optimized runtime keyword path and a lighter direct path for plain keyword strings, so the overhead is usually small.
- use packed ints when single tags already come from numeric constants or external metadata, or when you want the fastest path for a single tag
- use dotted tag-path strings when you want a nested value or assignment in one step. In Python, this can also be faster than repeated nested `Sequence` / `DataSet` API calls because the traversal stays inside one C++ path-parse/lookup call.
- use `dicom.Tag(...)` when you want an explicit reusable tag object
- `ds.Rows` is convenient during development or interactive exploration, and it also works well with tab completion in many interactive shells because `dir()` exposes present public standard keywords. But it raises `AttributeError` when the keyword is unknown or the element is missing. For production code, string/int/`Tag` keys are usually easier to handle explicitly.

## Reading values

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

Missing lookups return an object that evaluates to `False` rather than raising:

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

Malformed keyword/Tag strings return `False`.

### Same lookup in method form: get_dataelement(...)

`get_dataelement(...)` does the same lookup as `ds[...]`. Some codebases prefer the named method form when it reads more clearly:

```python
elem = ds.get_dataelement("PatientName")
if elem:
    print(elem.vr, elem.length, elem.value)
```

It uses the same behavior as `ds[...]` for missing elements.

### Partial-load continuation

Use `ensure_loaded(...)` when a partial read stopped before the tag you now need:

```python
df.ensure_loaded("Rows")
df.dataset.ensure_loaded(dicom.Tag("Columns"))
```

Accepted key types are:

- `Tag`
- packed `int` such as `0x00280010`
- keyword or Tag string such as `"Rows"` or `"(0028,0010)"`

Nested dotted tag-path strings are not supported by `ensure_loaded(...)`.

### Fast path: get_value()

Use `get_value()` for one-shot value reads where a default such as `None` represents a missing element:

```python
rows = ds.get_value("Rows")
window_center = ds.get_value("WindowCenter", default=None)
```

This is the shortest non-raising value path when you do not need the `DataElement` object.
If the element is missing, you get the `default` back.

`get_value()` does not implicitly continue partial loading. If a file-backed dataset was
loaded only up to an earlier tag, querying a later tag returns the currently available
state. Call `ensure_loaded(...)` first when you need a later tag such as `Rows` after a partial read.

The supplied default value is used only for missing elements. A data element with a zero-length value still returns a typed empty value:

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

### Return types from `DataElement.value` and `get_value()`

- `SQ` / `PX` -> `Sequence` / `PixelSequence`
- numeric-like VRs (`IS`, `DS`, `AT`, `FL`, `FD`, `SS`, `US`, `SL`, `UL`, `SV`, `UV`) -> `int`, `float`, `Tag`, or `list[...]`
- `PN` -> `PersonName` or `list[PersonName]` when parsing succeeds
- charset-aware text VRs -> UTF-8 `str` or `list[str]`
- charset decode or `PN` parse failure -> raw `bytes`
- binary VRs -> read-only `memoryview`

For data elements with zero-length values:

- numeric-like VRs return `[]`
- text VRs return `""`
- `PN` returns an empty `PersonName`
- binary VRs return an empty read-only `memoryview`
- `SQ` / `PX` return empty container objects

This matches the underlying C++ vector accessors: zero-length numeric-like values are treated as parseable empty vectors, not as missing values.

### Zero-length return matrix

The most important rule is that a data element with a zero-length value is still *present*. It does not use the `default` argument and it does not behave like a missing lookup.

In particular, some string VRs can normally have `VM > 1`, but a zero-length value still reads back as an empty scalar-style value because `vm()` is `0`, not `> 1`.

| VR family | Non-empty `VM == 1` | Non-empty `VM > 1` | Zero-length value |
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

At the C++ layer the same rules now apply to the vector accessors:

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

### Boolean checks and missing-element objects

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

### Typed read/write helpers

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

## Writing values

### Ensure-or-create lookup

`ensure_dataelement(...)` is the chaining-friendly "make sure this element exists" API:

```python
rows = ds.ensure_dataelement("Rows")
private_value = ds.ensure_dataelement(0x00090030, dicom.VR.US)
```

Rules:

- if the element already exists and `vr` is omitted or `None`, the existing element is returned unchanged
- if the element already exists and `vr` is explicit but different, the existing element is reset
  in place so the requested VR is guaranteed
- if the element is missing, a new zero-length element is inserted
- unlike `add_dataelement(...)`, this API only resets when an explicit VR must be enforced
- on partially loaded file-backed datasets, calling `ensure_dataelement(...)` for a tag whose
  data element has not been parsed yet raises instead of implicitly continuing the load
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

On partially loaded file-backed datasets, `set_value(...)` does not load through the target
tag. If the target data element has not been parsed yet, it raises just like
`add_dataelement(...)` / `ensure_dataelement(...)`.

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

This form is useful when the tag is private or when you want to override an existing non-sequence element VR before assigning a value.

Rules:

- if the element is missing, the supplied VR is used to create it
- if the element exists and already has that VR, the value is updated in place
- if the element exists with a different non-`SQ`/non-`PX` VR, the binding may replace that VR and then assign the value
- `VR.None`, `SQ`, and `PX` are not valid override targets for this form

This form does not provide rollback semantics. If it returns `False`, the dataset
remains valid but the destination element state is unspecified.

### Attribute assignment convenience path

```python
ds.Rows = 512
df.PatientName = pn
```

Attribute assignment remains a convenience path for standard keyword-based updates.
It is concise and works on both `DataSet` and forwarded `DicomFile` access.
Unlike `set_value(...)`, this path raises on assignment failure instead of returning `False`,
so it is usually better suited to development, notebooks, and interactive use than to
production code that wants explicit error handling.

## Utility operations

### Iteration and size

`for elem in ds` iterates over the present elements in that dataset.
`ds.size()` returns the element count for that dataset.
`len(df)` forwards the root-dataset size from `DicomFile`.

```pycon
>>> for elem in ds:
...     print(elem.tag, elem.vr, elem.length)
(0002,0010) UI 20
(0010,0010) PN 8
(0028,0010) US 2
>>> ds.size()
42
>>> len(df)
42
```

Use `ds.size()` when you already have the dataset object.
Use `len(df)` when you are still working from the file object.

### dump()

`dump()` returns a tab-separated human-readable dump string on both `DicomFile` and `DataSet`.

```python
full_text = df.dump(max_print_chars=80, include_offset=True)
compact_text = ds.dump(max_print_chars=40, include_offset=False)
```

- `max_print_chars` truncates long `VALUE` previews.
- `include_offset=False` removes the `OFFSET` column.
- On a file-backed root dataset, `dump()` also loads any unread remaining elements before formatting the dump.

A representative output looks like this:

```text
TAG	VR	LEN	VM	OFFSET	VALUE	KEYWORD
'00020010'	UI	20	1	132	'1.2.840.10008.1.2.1'	TransferSyntaxUID
'00100010'	PN	8	1	340	'Doe^Jane'	PatientName
'00280010'	US	2	1	702	512	Rows
```

With `include_offset=False`, the header and columns become:

```text
TAG	VR	LEN	VM	VALUE	KEYWORD
'00100010'	PN	8	1	'Doe^Jane'	PatientName
```

## Additional notes

### Performance note

- Keyword and tag lookups use a constant-time dictionary path.
- On large files, prefer targeted element access over full iteration in Python hot loops.

### Pixel transform metadata

Frame-aware metadata resolution for:

- `DicomFile.rescale_transform_for_frame(frame_index)`
- `DicomFile.window_transform_for_frame(frame_index)`
- `DicomFile.voi_lut_for_frame(frame_index)`
- `DicomFile.modality_lut_for_frame(frame_index)`

is documented in [Pixel Transform Metadata Resolution](../reference/pixel_transform_metadata.md).

### Runnable examples

- `examples/python/dataset_access_example.py`
- `examples/python/dump_dataset_example.py`

## Related docs

- C++ counterpart: [C++ DataSet Guide](cpp_dataset_guide.md)
- Input and output behavior: [File I/O](file_io.md)
- File-level API surface: [DicomFile Reference](../reference/dicomfile_reference.md)
- `DataElement` details: [DataElement Reference](../reference/dataelement_reference.md)
- `Sequence` traversal: [Sequence Reference](../reference/sequence_reference.md)
- Exceptions and failure categories: [Error Handling](error_handling.md)
- Decoded pixel output: [Pixel Decode](pixel_decode.md)
- Text VRs and `PN`: [Charset and Person Name](charset_and_person_name.md)
- Supporting Python types: [Python API Reference](../reference/python_reference.md)
- UID generation details: [Generating UID](generating_uid.md)
- Pixel encode limits: [Pixel Encode Constraints](../reference/pixel_encode_constraints.md)
