# C++ API Overview

C++ consumers only need `#include <dicom.h>` to access core types and the perfect-hash dictionary lookups. Add `using namespace dicom::literals;` to enable user-defined literals like `"PatientName"_tag`.

## Minimal example
```cpp
#include <dicom.h>
using namespace dicom::literals;

int main() {
  dicom::DataSet ds;
  ds.attach_to_file("sample.dcm");
  ds.ensure_loaded("Rows"_tag);

  const long rows = ds.get_value<long>("Rows"_tag, 0L);
  // use rows
}
```

## Preferred DataElement access pattern

- Use `dataset.get_value<T>(tag, default)` for one-shot typed reads.
- Use `dataset.get_value<std::string_view>(...)` or `dataset.get_value<std::vector<std::string_view>>(...)`
  when you want zero-copy text access and can honor the returned view lifetime.
- Use `dataset.set_value(tag, value)` for one-shot typed writes.
- Use `dataset[tag].to_xxx().value_or(default)` when you are already working with a `DataElement`.
- Use `if (auto& e = dataset[tag]; e)` only when element presence itself matters.
- Use `get_dataelement(...)` when you want an explicitly named lookup API or tag-path parsing.
  It returns `DataElement&` and yields a falsey element (`VR::None`) on miss.
- `set_value(...)` returns `false` on encode/assignment failure. The `DataSet` remains valid,
  but the destination element state is unspecified; callers that need rollback must restore it themselves.

```cpp
long rows = ds.get_value<long>("Rows"_tag, 0L);
bool ok = ds.set_value("StudyDescription"_tag, std::string_view("Example"));
auto desc_view = ds.get_value<std::string_view>("StudyDescription"_tag);
double slope = ds["RescaleSlope"_tag].to_double().value_or(1.0);
const auto& dose =
    ds.get_dataelement("RadiopharmaceuticalInformationSequence.0.RadionuclideTotalDose");
if (dose) {
  // consume dose
}
```

## Raw value bytes (no copy)

For raw byte access, use `value_span()` on a `DataElement`.
It returns `std::span<const std::uint8_t>` and does not allocate.

For text VRs that can be read without charset-owned copies, `get_value<std::string_view>()`
and `get_value<std::vector<std::string_view>>()` provide a similar zero-copy fast path.
Those views are tied to the underlying `DataElement` storage and become invalid if the
element is modified or removed.

```cpp
using namespace dicom::literals;

auto& pixel_data = ds["PixelData"_tag];
auto bytes = pixel_data.value_span();
// bytes.data(), bytes.size()
```

## Batch assignment + error collection

When writing many elements, you can keep a single status flag and collect detailed
failure reasons through a thread-local `BufferingReporter`.

```cpp
#include <dicom.h>
#include <diagnostics.h>
#include <memory>
using namespace dicom::literals;

dicom::DataSet ds;
auto reporter = std::make_shared<dicom::diag::BufferingReporter>(256);
dicom::diag::set_thread_reporter(reporter);

bool ok = true;
ok &= ds.add_dataelement("Rows"_tag, dicom::VR::US).from_long(512);
ok &= ds.add_dataelement("Columns"_tag, dicom::VR::US).from_long(-1);
ok &= ds.add_dataelement("BitsAllocated"_tag, dicom::VR::US).from_long(16);

if (!ok) {
  auto messages = reporter->take_messages();
  // inspect or surface messages
}

dicom::diag::set_thread_reporter(nullptr);
```

Note: `add_dataelement(...)` returns `DataElement&` and can still throw on validation/allocation errors.

## DataSet attachment methods

- `attach_to_file(const std::string& path)`: Opens `path` via `InFileStream`, sets the stream identifier to the path, and prepares the dataset for lazy reading. Metadata is parsed later (e.g., on the first `ensure_loaded`, iteration, or `operator[]`/`get_dataelement` call).
- `attach_to_memory(const std::uint8_t* data, std::size_t size, bool copy = true)`: Wraps a raw buffer. With `copy = true` (default) it copies the bytes; with `copy = false` it references the caller-owned buffer, which **must** outlive the `DataSet`.
- `attach_to_memory(const std::string& name, const std::uint8_t* data, std::size_t size, bool copy = true)`: Same as above, but the stream identifier becomes `name` (helpful for diagnostics).
- `attach_to_memory(std::string name, std::vector<std::uint8_t>&& buffer)`: Moves an owning buffer into the dataset; the identifier is `name`.
- `path() const`: Returns the current stream identifier (file path, provided name, or `<memory>`).

Note: These attachment calls are intended for the root `DataSet` (the object returned by `read_file`/`read_bytes` or constructed directly). Sub-datasets created internally reuse the parent stream and should not reattach.

## UID generation helpers

- `dicom::uid::generate_uid()`: create a base generated UID.
- `dicom::uid::Generated::append(component)`: append one UID component with fallback policy.
- Full behavior and Python equivalents: [Generating UID](generating_uid.md)

## Key types (Doxygen)

:::{doxygenstruct} dicom::Tag
:project: dicomsdl
:members: Tag, group, element, value, to_string, is_private, operator bool
:undoc-members:
:::

:::{doxygenstruct} dicom::VR
:project: dicomsdl
:members: VR, is_known, raw_code, str, is_string, is_binary, is_sequence, fixed_length, from_string, from_chars
:undoc-members:
:::

:::{doxygenclass} dicom::DataElement
:project: dicomsdl
:members: DataElement, tag, vr, length, offset, parent, is_present, is_missing, value_span, vm, storage_kind, sequence, pixel_sequence, as_sequence, as_pixel_sequence, reserve_value_bytes, set_value_bytes, adopt_value_bytes, from_long, from_uid_string, to_long, to_double, to_string_view, to_uid_string
:undoc-members:
:::

:::{doxygenclass} dicom::DataSet
:project: dicomsdl
:members: DataSet, attach_to_file, attach_to_memory, path, add_dataelement, remove_dataelement, operator[], get_dataelement, get_value, set_value, dump_elements, read_attached_stream, ensure_loaded, is_explicit_vr, transfer_syntax_uid, begin, end, cbegin
:undoc-members:
:::
