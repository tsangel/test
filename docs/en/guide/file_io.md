# File I/O

This page covers disk and memory input, partial loading, and the main output paths for files, bytes, and streams.

For DICOM JSON Model read/write, including `read_json(...)`,
`write_json(...)`, `BulkDataURI`, and `set_bulk_data(...)`, see
[DICOM JSON](dicom_json.md).

## How File I/O Works

- `read_file(...)` and `read_bytes(...)` create a `DicomFile` and parse input up to `load_until` immediately.
- `write_file(...)` and `write_bytes(...)` serialize a `DicomFile` object to a file or bytes.
- `write_with_transfer_syntax(...)` is the output-oriented transcode path for writing directly to a file or stream with a different transfer syntax. This is often what you want when changing pixel compression, for example to `HTJ2KLossless`. It does not mutate the source object first. In C++, the same API family also has `std::ostream` variants.

## Read from disk

**C++**

```cpp
#include <dicom.h>

auto file = dicom::read_file("in.dcm");
```

**Python**

```python
import dicomsdl as dicom

df = dicom.read_file("in.dcm")
```

Notes:
- Use `load_until` when the tags you need are near the front of the file and you do not need the unread tail right away. It can avoid parsing the full dataset up front and reduce read cost.
- Later-tag access does not implicitly continue parsing. In both C++ and Python, call `ensure_loaded(tag)` when you later need more. In C++, `ensure_loaded(...)` takes a `Tag` such as `"Rows"_tag`, `"(0028,FFFF)"_tag`, or `dicom::Tag(0x0028, 0x0010)`. In Python, `ensure_loaded(...)` accepts a `Tag`, a packed `int`, or a keyword string for a single tag; dotted tag-path strings are not supported.
- Use `keep_on_error=True` when you want partially read data kept instead of an immediate exception; then inspect `has_error` and `error_message`.
- In Python, `path` accepts `str` and `os.PathLike`. In C++, disk-path APIs such as `read_file(...)`, `write_file(...)`, and `write_with_transfer_syntax(...)` take `std::filesystem::path`.

## Read from memory

**C++**

```cpp
#include <dicom.h>
#include <vector>

std::vector<std::uint8_t> payload = /* full DICOM byte stream */;
auto file = dicom::read_bytes("in-memory in.dcm", std::move(payload));
```

**Python**

```python
from pathlib import Path
import dicomsdl as dicom

payload = Path("in.dcm").read_bytes()
df = dicom.read_bytes(payload, name="in-memory in.dcm")
```

Notes:
- `name` becomes the identifier reported by `path()` / `path` and by diagnostics.
- `load_until` behaves the same way for in-memory input: useful when you only need an early part of the dataset, but unread tail data is not implicitly loaded later.
- In Python, `read_bytes(..., copy=False)` keeps a reference to the caller buffer instead of copying. Keep that buffer alive and do not mutate it while the `DicomFile` still depends on it.
- In C++, `read_bytes(...)` can copy from a raw pointer or take ownership of a moved `std::vector<std::uint8_t>`.

## Staged Reads

**C++**

```cpp
#include <dicom.h>
using namespace dicom::literals;

dicom::ReadOptions opts;
opts.load_until = "0028,ffff"_tag;

auto file = dicom::read_file("in.dcm", opts);  // initial partial parse

auto& ds = file->dataset();
ds.ensure_loaded("PixelData"_tag);  // later, advance farther
```

**Python**

```python
import dicomsdl as dicom

df = dicom.read_file("in.dcm", load_until=dicom.Tag("0028,ffff"))
df.ensure_loaded("PixelData")
```

Notes:
- Set `options.load_until` when you want the initial parse to stop early.
- After a partial read, use `ensure_loaded(tag)` to parse more data elements. In C++, pass a `Tag` such as `"Rows"_tag`, `"(0028,FFFF)"_tag`, or `dicom::Tag(...)`.
- On partially loaded datasets, data elements that have not been parsed yet are not implicitly loaded for later lookups or writes.
- In Python, `ensure_loaded(...)` accepts a `Tag`, a packed `int`, or a keyword string for a single tag. Nested dotted tag-path strings are not supported.
- The same staged-read pattern works with `read_bytes(...)`; use `copy=false` when you want zero-copy memory input.
- With `read_bytes(..., copy=false)`, the caller-owned buffer must outlive the `DicomFile`.

## Partial loading and permissive read

- `load_until` stops parsing after the requested tag is read, inclusive.
- `keep_on_error` keeps partially read data and records the read failure on the `DicomFile`.
- On partially loaded datasets loaded from file or memory, lookup and mutation APIs do not implicitly continue loading data elements that have not been parsed yet.
- In practice, this means later-tag access may behave as missing or raise, and later-tag writes may raise instead of silently mutating unread data.

**C++**

```cpp
#include <dicom.h>
using namespace dicom::literals;

dicom::ReadOptions opts;
opts.load_until = "0002,ffff"_tag;  // stop after file meta

auto file = dicom::read_file("in.dcm", opts);
auto& ds = file->dataset();

ds.ensure_loaded("0028,0011"_tag);  // advance through Columns

long rows = ds.get_value<long>("0028,0010"_tag, -1L);  // Rows
long cols = ds.get_value<long>("0028,0011"_tag, -1L);  // Columns
long bits = ds.get_value<long>("0028,0100"_tag, -1L);  // BitsAllocated

// rows and cols are now available
// bits is still -1 because (0028,0100) has not been parsed yet

ds.ensure_loaded("0028,ffff"_tag);
bits = ds.get_value<long>("0028,0100"_tag, -1L);  // now available
```

Notes:
- This is useful when the tags you need are clustered near the front of the dataset.
- It is also a good fit for fast scans across many DICOM files, such as building a metadata index or database without touching the full dataset or pixel payload.
- Python supports the same `ensure_loaded(...)` continuation pattern for single tags and keywords.

## Serialize a `DicomFile` object to a file or bytes

**C++**

```cpp
#include <dicom.h>

auto file = dicom::read_file("in.dcm");

dicom::WriteOptions opts;
opts.include_preamble = true;
opts.write_file_meta = true;
opts.keep_existing_meta = false;

file->write_file("out.dcm", opts);
auto payload = file->write_bytes(opts);
```

**Python**

```python
import dicomsdl as dicom

df = dicom.read_file("in.dcm")
payload = df.write_bytes(keep_existing_meta=False)
df.write_file("out.dcm", keep_existing_meta=False)
```

Notes:
- With default options, `write_file()` and `write_bytes()` produce a normal Part 10 style output with preamble and file meta information.
- `write_file_meta=False` omits the file meta group.
- `include_preamble=False` omits the 128-byte preamble.
- `keep_existing_meta=False` rebuilds file meta before writing. Use `rebuild_file_meta()` when you want that step to happen explicitly before serialization.
- These APIs serialize a `DicomFile` object to a file or bytes. They do not provide a separate output-only transcode path.

## Transcode directly to a file or stream

**C++**

```cpp
#include <dicom.h>

auto file = dicom::read_file("in.dcm");
file->write_with_transfer_syntax(
    "out_htj2k_lossless.dcm",
    dicom::uid::WellKnown::HTJ2KLossless
);
```

**Python**

```python
from pathlib import Path
import dicomsdl as dicom

df = dicom.read_file("in.dcm")
df.write_with_transfer_syntax(Path("out_htj2k_lossless.dcm"), "HTJ2KLossless")
```

Notes:
- `write_with_transfer_syntax(...)` transcodes directly to the output using a target transfer syntax. This is often used to change pixel compression, for example to `HTJ2KLossless`, without mutating the source `DicomFile`.
- Prefer it when the real goal is an output file or stream, especially for large pixel payloads. It can reduce peak memory use by avoiding an in-memory transcode path that keeps both decode working buffers and the re-encoded target `PixelData` alive longer than needed.
- In Python, `write_with_transfer_syntax(...)` is the path-based output-only transcode API. In C++, the same API family also supports direct stream outputs.
- Seekable outputs can backpatch `ExtendedOffsetTable` data when needed. Non-seekable outputs remain valid DICOM, but may omit those tables and use an empty Basic Offset Table.
- Typical seekable outputs are regular files on local disk. Typical non-seekable outputs are pipes, sockets, stdout, HTTP response streams, or zip-entry style streams.

## Which API should I use?

- Local file, parse up to the requested boundary immediately: `read_file(...)`
- Bytes already in memory: `read_bytes(...)`
- Zero-copy memory input in Python: `read_bytes(..., copy=False)`
- File-backed staged read in C++: `read_file(...)` with `load_until`, then `ensure_loaded(...)`
- Zero-copy staged read from memory in C++: `read_bytes(...)` with `copy=false` and optional `load_until`, then `ensure_loaded(...)`
- Serialize a `DicomFile` object to a file or bytes: `write_file(...)` or `write_bytes(...)`
- Write a new transfer syntax directly to a path: `write_with_transfer_syntax(...)`
- In C++, write a new transfer syntax directly to an output stream: `write_with_transfer_syntax(...)`

## Related docs

- [DICOM JSON](dicom_json.md)
- [Core Objects](core_objects.md)
- [C++ DataSet Guide](cpp_dataset_guide.md)
- [Python DataSet Guide](python_dataset_guide.md)
- [Pixel Decode](pixel_decode.md)
- [Pixel Encode](pixel_encode.md)
- [C++ API Overview](../reference/cpp_api.md)
- [DataSet Reference](../reference/dataset_reference.md)
- [DicomFile Reference](../reference/dicomfile_reference.md)
- [Error Handling](error_handling.md)
