# C++ DataSet Guide

This is the main user-facing guide for C++-side dataset and element work in DicomSDL.
It covers how the main objects relate, how to spell tags, and the most important read/write patterns.

## How DicomSDL maps to DICOM

DicomSDL exposes a small set of related C++ objects:

- `DicomFile`: file/session wrapper that owns the root dataset
- `DataSet`: container of DICOM `DataElement` objects
- `DataElement`: one DICOM field, with tag / VR / length metadata and typed/raw value access
- `Sequence`: nested item container for `SQ` values
- `PixelSequence`: frame / fragment container for encapsulated or compressed pixel data

For the object model and DICOM mapping, see [Core Objects](core_objects.md).

C++ does not provide Python-style attribute convenience access.
Tags, keywords, and dotted tag paths stay explicit.

### DicomFile and DataSet

Most data element access APIs are implemented on `DataSet`.
`DicomFile` owns the root `DataSet` and handles file-oriented work such as load, save, and transcode.
For convenience, `DicomFile` forwards many root-dataset helpers such as `get_value(...)`,
`get_dataelement(...)`, `set_value(...)`, `ensure_dataelement(...)`, and `ensure_loaded(...)`.

For a few root-level reads mixed with file-level operations, `DicomFile` forwarding is often enough:

```cpp
#include <dicom.h>
using namespace dicom::literals;

auto file = dicom::read_file("sample.dcm");

long rows1 = file->get_value<long>("Rows"_tag, 0L);
const auto& patient_name1 = file->get_dataelement("PatientName"_tag);
```

For repeated dataset work, taking `DataSet` explicitly is usually clearer:

```cpp
#include <dicom.h>
using namespace dicom::literals;

auto file = dicom::read_file("sample.dcm");
auto& ds = file->dataset();

long rows2 = ds.get_value<long>("Rows"_tag, 0L);
const auto& patient_name2 = ds["PatientName"_tag];
```

## Recommended API surface

| API | Returns | Missing behavior | Intended use |
| --- | --- | --- | --- |
| `ds.get_value<long>("Rows"_tag)` | `std::optional<long>` | `std::nullopt` | typed read where `std::nullopt` means missing |
| `ds.get_value<long>("Rows"_tag, 0L)` | `long` | returns the supplied default value | one-shot typed read |
| `ds["Rows"_tag]`, `ds["Rows"]`, `ds.get_dataelement("Rows")` | `DataElement&` | returns `VR::None` and evaluates to `false` | typed read plus metadata access |
| `if (const auto& e = ds["Rows"_tag]; e)` | branch on presence | `false` on miss | presence-sensitive code |
| `ds.ensure_loaded("(0028,FFFF)"_tag)` | `void` | throws on invalid use | explicitly continue a partial read to a later tag boundary |
| `ds.ensure_dataelement("Rows"_tag, dicom::VR::US)` | `DataElement&` | returns existing or inserts | chaining-friendly ensure/create |
| `ds.set_value("Rows"_tag, 512L)` | `bool` | `false` on encode/assignment failure | one-shot ensure + typed assignment |
| `ds.add_dataelement("Rows"_tag, dicom::VR::US)` | `DataElement&` | create/replace | explicit leaf insertion |

## Ways to spell a Tag in C++

The user-defined literal suffix is `"_tag"` in lowercase, not `"_Tag"`.

| Form | Example | Use first when |
| --- | --- | --- |
| keyword literal | `"Rows"_tag` | most standard tags in normal C++ code |
| numeric tag literal | `"(0028,0010)"_tag` | the numeric tag spelling is the clearest form |
| group/element constructor | `dicom::Tag(0x0028, 0x0010)` | group and element already exist as separate values |
| packed-tag constructor | `dicom::Tag(0x00280010)` | the tag already exists as packed `0xGGGGEEEE` data |
| runtime text parse | `dicom::Tag("Rows")`, `dicom::Tag("(0028,0010)")` | a keyword or numeric tag arrives as runtime text |
| string/path form | `ds["Rows"]`, `ds.get_value<double>("00540016.0.00181074")` | you want a keyword lookup or one-shot nested read/write path |

### `"Rows"_tag`

- Use this first for most standard tags in everyday C++ code.
- Advantages: short, readable, compile-time checked, no runtime parse.
- Tradeoffs: only works with string literals known at compile time.

### `"(0028,0010)"_tag`

- Use this when the numeric tag spelling is more recognizable than the keyword.
- Advantages: unambiguous, compile-time checked, works in Tag-only APIs.
- Tradeoffs: more verbose than a keyword, easier to mistype.

### `dicom::Tag(0x0028, 0x0010)`

- Use this when group and element already exist as separate runtime values.
- Advantages: explicit, works with runtime values, no text parsing.
- Tradeoffs: more verbose than a keyword literal.

### `dicom::Tag(0x00280010)`

- Use this when the tag already exists as a packed `0xGGGGEEEE` tag value.
- Advantages: compact when integrating with generated tables or packed tag values.
- Tradeoffs: bare `0x00280010` is not accepted by `DataSet` APIs; wrap it in `Tag(...)` or `Tag::from_value(...)`.

### `dicom::Tag("Rows")` or `dicom::Tag("(0028,0010)")`

- Use this when a keyword or numeric tag arrives as runtime text.
- Advantages: accepts either keyword text or numeric tag text.
- Tradeoffs: parses at runtime and can throw on invalid text.

### String/path forms

- Use this when you want keyword lookup, dotted tag-path traversal, or one-shot nested read/write code.
- Examples: `ds["Rows"]`, `ds.get_value<double>("00540016.0.00181074")`, `ds.set_value("PatientName", "Doe^Jane")`, `ds.ensure_dataelement("ReferencedStudySequence.0.ReferencedSOPInstanceUID", dicom::VR::UI)`.
- Advantages: no explicit `Tag` construction, supports nested paths across `operator[]`, `get_dataelement(...)`, `get_value(...)`, `set_value(...)`, `ensure_dataelement(...)`, and `add_dataelement(...)`.
- Tradeoffs: string parsing happens at runtime.

Practical recommendation:

- use `"Rows"_tag` for usual tag access in most C++ code
- use `"(0028,0010)"_tag` when the numeric tag is the clearest spelling
- use `dicom::Tag(...)` when the tag comes from runtime integers or packed values
- use string/path forms when you want a nested lookup or write in one step, for example `ds.get_value<double>("RadiopharmaceuticalInformationSequence.0.RadionuclideTotalDose")`, `ds.get_value<double>("00540016.0.00181074")`, `ds.set_value("ReferencedStudySequence.0.ReferencedSOPInstanceUID", "1.2.3")`, or `ds.ensure_dataelement("ReferencedStudySequence.0.ReferencedSOPInstanceUID", dicom::VR::UI)`

## Reading values

### Fast path: get_value<T>()

Use `get_value<T>()` when you only need the typed value:

```cpp
#include <dicom.h>
using namespace dicom::literals;

auto file = dicom::read_file("sample.dcm");
auto& ds = file->dataset();

if (auto rows = ds.get_value<long>("Rows"_tag)) {
  // use *rows
}

double slope = ds.get_value<double>("RescaleSlope"_tag, 1.0);
auto desc = ds.get_value<std::string_view>("StudyDescription"_tag);
```

- `get_value<T>(tag)` returns `std::optional<T>`. Use this when you want to distinguish "missing" from "present with a real value" in the caller.
- `get_value<T>(tag, default_value)` returns `T`. Use this when you want an inline fallback and do not need to distinguish an empty result from the fallback path.
- The default-value form is effectively `get_value<T>(...).value_or(default_value)`.
- `get_value<std::string_view>(...)` is a zero-copy view. Keep the owning dataset/file alive while you use it.

### DataElement access: operator[]

Use `operator[]` when you want a `DataElement` and not just the value:

```cpp
#include <dicom.h>
using namespace dicom::literals;

auto file = dicom::read_file("sample.dcm");
auto& ds = file->dataset();

const auto& rows_elem = ds["Rows"];
if (rows_elem) {
  long rows = rows_elem.to_long(0L);
}

const auto& dose_elem =
    ds["RadiopharmaceuticalInformationSequence.0.RadionuclideTotalDose"];
```

`operator[]` accepts a `Tag`, a keyword string, or a dotted tag path.
Use `"_tag"` when you want compile-time spelling with no runtime parse.

### Presence checks

Use a boolean check on the returned `DataElement` when element presence itself matters:

```cpp
if (const auto& rows_elem = ds["Rows"_tag]; rows_elem) {
  long rows = rows_elem.to_long(0L);
}

if (const auto& patient_name = ds.get_dataelement("PatientName"); patient_name) {
  // present
}
```

Missing lookups return a `DataElement` with `VR::None` that evaluates to `false` rather than throwing.

### Same lookup in method form: get_dataelement(...)

`get_dataelement(...)` does the same lookup as `operator[]`. Some codebases prefer the method spelling when a named function reads more clearly than `ds[...]`:

```cpp
const auto& dose = ds.get_dataelement(
    "RadiopharmaceuticalInformationSequence.0.RadionuclideTotalDose");
if (dose) {
  double value = dose.to_double(0.0);
}
```

### Partial-load continuation

Use `ensure_loaded(tag)` when a partial read stopped before the tag you now need:

```cpp
ds.ensure_loaded("(0028,FFFF)"_tag);

long rows = ds.get_value<long>("Rows"_tag, 0L);
long cols = ds.get_value<long>("Columns"_tag, 0L);
```

`ensure_loaded(...)` takes a `Tag`, such as `"Rows"_tag`, `"(0028,FFFF)"_tag`, or `dicom::Tag(0x0028, 0x0010)`.
It does not take keyword strings or dotted tag paths.

### Return types and zero-length behavior

The main typed read families are:

- scalar numeric: `to_int()`, `to_long()`, `to_longlong()`, `to_double()`
- vector numeric/tag: `to_longlong_vector()`, `to_double_vector()`, `to_tag_vector()`
- text: `to_string_view()`, `to_string_views()`, `to_utf8_string()`, `to_utf8_strings()`
- tags and UIDs: `to_tag()`, `to_uid_string()`, `to_transfer_syntax_uid()`
- person names: `to_person_name()`, `to_person_names()`

For vector accessors, zero-length values return an engaged empty container rather than `std::nullopt`:

```cpp
auto rows = ds["Rows"_tag].to_longlong_vector();         // empty vector when zero-length
auto wc = ds["WindowCenter"_tag].to_double_vector();     // empty vector when zero-length
auto at = ds["FrameIncrementPointer"_tag].to_tag_vector();
```

For scalar accessors, treat `std::nullopt` as "not available from this accessor" and use the
`DataElement` itself when you need to distinguish missing from present zero-length values.

### Distinguishing zero-length from missing

In DicomSDL, `missing` and `zero-length` are different element states and should be tested at the `DataElement` level:

```cpp
const auto& elem = ds["PatientName"_tag];

if (!elem) {
  // missing lookup
} else if (elem.length() == 0) {
  // present element with zero-length value
} else {
  // present element with non-empty value
}
```

Practical differences:

- missing element
  - `bool(elem) == false`
  - `elem.is_missing() == true`
  - `elem.vr() == dicom::VR::None`
- zero-length present element
  - `bool(elem) == true`
  - `elem.is_missing() == false`
  - `elem.vr() != dicom::VR::None`
  - `elem.length() == 0`

## DataElement

`DataElement` is the main metadata-bearing object.

### Core properties

- `elem.tag()`
- `elem.vr()`
- `elem.length()`
- `elem.offset()`
- `elem.vm()`
- `elem.parent()`

```cpp
const auto& elem = ds["Rows"_tag];
auto tag = elem.tag();
auto vr = elem.vr();
auto length = elem.length();
```

### Boolean checks and missing-element objects

```cpp
const auto& elem = ds["PatientName"_tag];
if (elem) {
  // present
}

const auto& missing = ds["NotARealKeyword"];
if (!missing && missing.is_missing()) {
  // missing lookup
}
```

For zero-length present elements, `bool(elem)` is still `true`; use `elem.length() == 0` to detect them.

### Typed read/write helpers

- `to_long()`, `to_double()`, `to_tag()`, `to_uid_string()`
- `to_string_view()`, `to_utf8_string()`, `to_utf8_strings()`
- `to_person_name()`, `to_person_names()`
- `from_long(...)`, `from_double(...)`, `from_tag(...)`
- `from_string_view(...)`, `from_utf8_view(...)`, `from_uid_string(...)`
- `from_person_name(...)`, `from_person_names(...)`

When you already have a `DataElement&`, the `from_xxx(...)` helpers are the direct write path.

### Container helpers

For `SQ` and encapsulated pixel data:

- `elem.sequence()` / `elem.as_sequence()`
- `elem.pixel_sequence()` / `elem.as_pixel_sequence()`

Treat these as container values, not as scalar strings or numbers.

### Raw bytes and view lifetimes

`value_span()` returns `std::span<const std::uint8_t>` without copying:

```cpp
const auto& pixel_data = ds["PixelData"_tag];
auto bytes = pixel_data.value_span();
// bytes.data(), bytes.size()
```

`to_string_view()` style accessors are also view-based.
Views become invalid if the element is replaced or mutated, so keep the owning dataset/file alive and refresh views after writes.

## Writing values

### ensure_dataelement(...)

Use `ensure_dataelement(...)` when you want chaining-friendly ensure/create behavior:

```cpp
auto& existing_rows = ds.ensure_dataelement("Rows"_tag);  // default vr == VR::None
ds.ensure_dataelement("Rows"_tag, dicom::VR::US).from_long(512);
ds.ensure_dataelement(
    "ReferencedStudySequence.0.ReferencedSOPInstanceUID",
    dicom::VR::UI).from_uid_string("1.2.3");
```

Rules:

- existing element + omitted `vr` (default `VR::None`) or explicit `VR::None` -> preserve as-is
- existing element + explicit different VR -> reset in place
- missing element + explicit VR -> insert zero-length element with that VR
- missing standard tag + omitted `vr` -> insert zero-length element with dictionary VR
- missing unknown/private tag + omitted `vr` -> throw, because there is no dictionary VR to resolve

### Update an existing element through the returned DataElement

Use `from_xxx(...)` on a `DataElement` when you already have the element:

```cpp
if (auto& rows = ds["Rows"_tag]; rows) {
  rows.from_long(512);
}
```

If you want create-or-update behavior, start from `ensure_dataelement(...)` instead of `operator[]`.

### One-shot assignment with set_value(...)

Use `set_value(...)` when you want the same ensure + typed-write flow in one call:

```cpp
bool ok = true;
ok &= ds.set_value("Rows"_tag, 512L);
ok &= ds.set_value("Columns"_tag, 512L);
ok &= ds.set_value("BitsAllocated"_tag, 16L);
ok &= ds.set_value(dicom::Tag(0x0009, 0x0030), dicom::VR::US, 16L);  // private tag
```

It follows the same `ensure_dataelement(...)` rules above for existing elements, missing elements,
and explicit vs omitted `vr`, then returns `false` on encode or assignment failure.

Failure model:

- on success, the requested value is written
- on failure, `set_value()` returns `false`
- the `DataSet` / `DicomFile` remains usable
- the destination element state is unspecified and should not be relied on

If you need rollback semantics, keep the previous value yourself and restore it explicitly.

### Creating zero-length values vs removing elements

Zero-length and removal are different operations.

Create or preserve a zero-length present element with `add_dataelement(...)`, `ensure_dataelement(...)`,
or an explicit empty payload:

```cpp
ds.add_dataelement("PatientName"_tag, dicom::VR::PN);  // present, zero-length
ds.set_value("PatientName"_tag, std::string_view{});

std::vector<long long> empty_numbers;
ds.set_value("Rows"_tag, std::span<const long long>(empty_numbers));
```

Use `remove_dataelement(...)` for deletion:

```cpp
ds.remove_dataelement("PatientName"_tag);
ds.remove_dataelement(dicom::Tag(0x0028, 0x0010));
```

### Explicit VR assignment for private or ambiguous tags

```cpp
bool ok = ds.set_value(dicom::Tag(0x0009, 0x0030), dicom::VR::US, 16L);
```

This form is useful when the tag is private or when you want to override an existing
non-sequence element VR before assigning a value.

Rules:

- if the element is missing, the supplied VR is used to create it
- if the element exists and already has that VR, the value is updated in place
- if the element exists with a different non-`SQ`/non-`PX` VR, the binding may replace that VR and then assign the value
- `VR::None`, `SQ`, and `PX` are not valid override targets for this form

### add_dataelement(...)

Use `add_dataelement(...)` when you want explicit create/replace semantics:

```cpp
ds.add_dataelement("Rows"_tag, dicom::VR::US).from_long(512);
```

Compared with `ensure_dataelement(...)`, `add_dataelement(...)` is more destructive for existing elements.
If the target element is already present, `add_dataelement(...)` resets it to a new zero-length element before you fill it again.

Use `add_dataelement(...)` when that explicit replace behavior is what you want.
If you want to preserve an existing element unless an explicit VR change is required, use `ensure_dataelement(...)` instead.
`set_value(...)` follows the `ensure_dataelement(...)` path, not the `add_dataelement(...)` path.

## Utility operations

### Iteration and size

`for (const auto& elem : ds)` iterates over the present elements in that dataset.
`ds.size()` returns the element count for that dataset.
`file->size()` forwards the root-dataset size from `DicomFile`.

```cpp
for (const auto& elem : ds) {
  std::cout << elem.tag().to_string()
            << ' ' << elem.vr().str()
            << ' ' << elem.length() << '\n';
}

std::cout << "element count: " << ds.size() << '\n';
std::cout << "file count: " << file->size() << '\n';
```

A representative output looks like this:

```text
(0002,0010) UI 20
(0010,0010) PN 8
(0028,0010) US 2
element count: 42
file count: 42
```

### dump()

`dump()` returns a tab-separated human-readable dump string on both `DataSet` and `DicomFile`.

```cpp
auto full = file->dump(80, true);
auto compact = ds.dump(40, false);
```

- `max_print_chars` truncates long `VALUE` previews.
- `include_offset = false` removes the `OFFSET` column.
- On a file-backed root dataset, `dump()` also loads any unread remaining elements before formatting the dump.

A representative output looks like this:

```text
TAG	VR	LEN	VM	OFFSET	VALUE	KEYWORD
'00020010'	UI	20	1	132	'1.2.840.10008.1.2.1'	TransferSyntaxUID
'00100010'	PN	8	1	340	'Doe^Jane'	PatientName
'00280010'	US	2	1	702	512	Rows
```

With `include_offset = false`, the header and columns become:

```text
TAG	VR	LEN	VM	VALUE	KEYWORD
'00100010'	PN	8	1	'Doe^Jane'	PatientName
```

## Partial-load rules

- `get_value(...)`, `get_dataelement(...)`, and `operator[]` do not implicitly continue a partial load.
- Data elements that have not been parsed yet behave as missing until you call `ensure_loaded(tag)`.
- `add_dataelement(...)`, `ensure_dataelement(...)`, and `set_value(...)` raise when the target data element has not been parsed yet.
- When you need later tags after a staged read, explicitly move the load boundary first and then read or write.

## Additional notes

### Performance note

- For usual single-tag access in hot paths, prefer `"_tag"` literals or reused `dicom::Tag` objects over runtime text parsing.
- Prefer `get_value<T>(tag, default)` when you only need the typed value and do not need `DataElement` metadata.
- Use string/path forms when they make nested access clearer. For repeated hot-loop lookups, cache the tag or split the traversal explicitly.

### Runnable examples

- `examples/dataset_access_example.cpp`
- `examples/batch_assign_with_error_check.cpp`
- `examples/dump_dataset_example.cpp`
- `examples/tag_lookup_example.cpp`
- `examples/keyword_lookup_example.cpp`

## Related docs

- [Core Objects](core_objects.md)
- [File I/O](file_io.md)
- [Sequence and Paths](sequence_and_paths.md)
- [Python DataSet Guide](python_dataset_guide.md)
- [C++ API Overview](../reference/cpp_api.md)
- [DataSet Reference](../reference/dataset_reference.md)
- [DataElement Reference](../reference/dataelement_reference.md)
- [Error Handling](error_handling.md)
