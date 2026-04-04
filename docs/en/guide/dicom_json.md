# DICOM JSON

DicomSDL can serialize a `DataSet` or `DicomFile` to the DICOM JSON Model and
can read DICOM JSON back into one or more `DicomFile` objects.

This page focuses on the public `write_json(...)`, `read_json(...)`, and
`set_bulk_data(...)` workflows in both Python and C++.

## What is supported

- `DicomFile.write_json(...)`
- `DataSet.write_json(...)`
- `read_json(...)` from UTF-8 text or bytes already in memory
- DICOM JSON top-level object and top-level array payloads
- `BulkDataURI`, `InlineBinary`, nested sequences, and PN objects
- caller-managed bulk download via `JsonBulkRef` + `set_bulk_data(...)`

Current scope notes:

- `read_json(...)` is a memory-input API. It does not stream from disk or HTTP.
- The JSON reader/writer implements the DICOM JSON Model, not a full DICOMweb
  HTTP client or server stack.

## Write JSON

### Python write example

```python
import dicomsdl as dicom

df = dicom.read_file("sample.dcm")

json_text, bulk_parts = df.write_json()
```

The return value is:

- `json_text: str`
- `bulk_parts: list[tuple[str, memoryview, str, str]]`

Each bulk tuple is:

- `uri`
- `payload`
- `media_type`
- `transfer_syntax_uid`

### C++ write example

```cpp
#include <dicom.h>

auto file = dicom::read_file("sample.dcm");
dicom::JsonWriteResult out = file->write_json();

std::string json_text = std::move(out.json);
for (const auto& part : out.bulk_parts) {
    auto bytes = part.bytes();
    // part.uri
    // part.media_type
    // part.transfer_syntax_uid
}
```

## JSON write options

`JsonWriteOptions` is defined in the public headers and exposed through Python
keyword arguments.

### `include_group_0002`

- Default: `false`
- Meaning: include file meta group `0002` in the JSON output

By default, DICOM JSON / DICOMweb style output excludes group `0002`. Group
length elements `(gggg,0000)` are always excluded.

### `bulk_data`

Python values:

- `"inline"`
- `"uri"`
- `"omit"`

C++ values:

- `JsonBulkDataMode::inline_`
- `JsonBulkDataMode::uri`
- `JsonBulkDataMode::omit`

Behavior:

- `inline`: bulk-capable values stay inline as `InlineBinary`
- `uri`: values at or above the threshold move to `BulkDataURI`
- `omit`: the attribute stays present with `vr`, but the bulk value itself is
  not emitted

### `bulk_data_threshold`

- Default: `1024`
- Used only with `bulk_data="uri"`

When `bulk_data="uri"`, values smaller than the threshold stay inline and
values at or above the threshold become `BulkDataURI`.

### `bulk_data_uri_template`

Template used for non-PixelData bulk elements when `bulk_data="uri"`.

Supported placeholders:

- `{study}`
- `{series}`
- `{instance}`
- `{tag}`

`{tag}` expands to:

- top-level element: `7FE00010`
- nested sequence element: dotted tag path such as `22002200.0.12340012`

Example:

```python
json_text, bulk_parts = df.write_json(
    bulk_data="uri",
    bulk_data_uri_template="/dicomweb/studies/{study}/series/{series}/instances/{instance}/bulk/{tag}",
)
```

### `pixel_data_uri_template`

Optional override for `PixelData (7FE0,0010)`.

Typical use:

```python
json_text, bulk_parts = df.write_json(
    bulk_data="uri",
    bulk_data_uri_template="/dicomweb/studies/{study}/series/{series}/instances/{instance}/bulk/{tag}",
    pixel_data_uri_template="/dicomweb/studies/{study}/series/{series}/instances/{instance}/frames",
)
```

Use this when your server exposes frame-oriented pixel routes separately from
other bulk data.

### Write `charset_errors`

Python values:

- `"strict"`
- `"replace_fffd"`
- `"replace_hex_escape"`

C++ values:

- `CharsetDecodeErrorPolicy::strict`
- `CharsetDecodeErrorPolicy::replace_fffd`
- `CharsetDecodeErrorPolicy::replace_hex_escape`

This controls text decode handling while producing JSON text.

## PixelData bulk behavior

### Native PixelData

- JSON keeps one `BulkDataURI`
- native multi-frame bulk stays one aggregate bulk part

### Encapsulated PixelData

- JSON still keeps one base `BulkDataURI`
- `bulk_parts` are returned frame by frame
- frame URIs follow the chosen base:
  - `/.../frames` -> `/.../frames/1`, `/.../frames/2`, ...
  - generic base URI -> `/.../bulk/7FE00010/frames/1`, ...

This keeps the JSON compact while still giving callers a per-frame payload list
for multipart or frame-response assembly.

## Read JSON

### Python read example

```python
import dicomsdl as dicom

items = dicom.read_json(json_text)

for df, refs in items:
    ...
```

### C++ read example

```cpp
#include <dicom.h>

dicom::JsonReadResult result = dicom::read_json(
    reinterpret_cast<const std::uint8_t*>(json_bytes.data()),
    json_bytes.size());

for (auto& item : result.items) {
    auto& file = *item.file;
    auto& refs = item.pending_bulk_data;
    (void)file;
    (void)refs;
}
```

The reader always returns a collection because DICOM JSON may contain:

- one dataset object
- an array of dataset objects

If the JSON is a single top-level object, the result list has length `1`.

## JSON read options

### Read `charset_errors`

Python values:

- `"strict"`
- `"replace_qmark"`
- `"replace_unicode_escape"`

C++ values:

- `CharsetEncodeErrorPolicy::strict`
- `CharsetEncodeErrorPolicy::replace_qmark`
- `CharsetEncodeErrorPolicy::replace_unicode_escape`

This policy is used when text parsed from UTF-8 JSON later needs to be
converted back into raw DICOM bytes for APIs such as `value_span()`,
`write_file(...)`, or `set_bulk_data(...)`.

## Bulk download flow

Typical Python flow:

```python
items = dicom.read_json(json_text)

for df, refs in items:
    for ref in refs:
        payload = download(ref.uri)
        df.set_bulk_data(ref, payload)
```

Typical C++ flow:

```cpp
for (auto& item : result.items) {
    for (const auto& ref : item.pending_bulk_data) {
        std::vector<std::uint8_t> payload = download(ref.uri);
        item.file->set_bulk_data(ref, payload);
    }
}
```

`JsonBulkRef` contains:

- `kind`
- `path`
- `frame_index`
- `uri`
- `media_type`
- `transfer_syntax_uid`
- `vr`

## How URI preservation works on read

The JSON reader is intentionally conservative.

It preserves already-dereferenceable URIs such as:

- `.../frames/1`
- `.../frames/1,2,3`
- presigned, tokenized, or opaque absolute URLs such as
  `https://example.test/instances/1?sig=...`
- presigned or tokenized generic pixel URLs such as
  `https://example.test/studies/s/series/r/instances/i/bulk/7FE00010?sig=...`

It only synthesizes frame URLs when the URI shape itself makes the frame route
explicit, for example:

- `.../frames`
- plain generic base URIs without a signature or token suffix, such as `.../bulk/7FE00010`

This matters for presigned or tokenized download URLs: appending `/frames/{n}`
to an already-signed opaque URL changes the path and usually breaks
dereferencing, so those URLs are kept unchanged.

## `set_bulk_data(...)` behavior

`set_bulk_data(...)` supports two important cases:

- frame refs: copy one encoded frame into an encapsulated PixelData slot
- opaque encapsulated element refs: accept the whole encapsulated PixelData
  value field and rebuild it as a writable internal pixel sequence

That means an opaque presigned or tokenized `BulkDataURI` can still participate
in the normal flow:

1. `read_json(...)` preserves the presigned or tokenized download URL as one
   `element` ref
2. caller downloads the payload bytes from that URL
3. `set_bulk_data(ref, payload)` rebuilds encapsulated PixelData from the
   downloaded value field

## Transfer syntax notes

`JsonBulkPart.transfer_syntax_uid` and `JsonBulkRef.transfer_syntax_uid` are
populated from file meta `TransferSyntaxUID (0002,0010)` when it is present.
If that information is not present, the reader stays conservative and avoids
guessing encapsulated frame layout from metadata alone.

## Input rules

- JSON input must be UTF-8 text
- Python accepts `str` or bytes-like input
- empty input is an error
- top-level input must be a JSON object or array

## Related docs

- [File I/O](file_io.md)
- [Python DataSet Guide](python_dataset_guide.md)
- [Python API Reference](../reference/python_reference.md)
- [DicomFile Reference](../reference/dicomfile_reference.md)
- [DataSet Reference](../reference/dataset_reference.md)
