# C++ API Overview

C++ consumers only need `#include <dicom.h>` to access core types and the perfect-hash dictionary lookups. Add `using namespace dicom::literals;` to enable user-defined literals like `"PatientName"_tag`.

## Minimal example
```cpp
#include <dicom.h>
using namespace dicom::literals;

int main() {
  dicom::DataSet ds;
  ds.attach_to_file("sample.dcm");

  constexpr dicom::Tag rows = "Rows"_tag;
  const auto elem = ds.get_dataelement(rows);
  if (elem) {
    // access length, vr, etc.
  }
}
```

## DataSet attachment methods

- `attach_to_file(const std::string& path)`: Opens `path` via `InFileStream`, sets the stream identifier to the path, and prepares the dataset for lazy reading. Metadata is parsed later (e.g., on the first `ensure_loaded`, iteration, or `get_dataelement` call).
- `attach_to_memory(const std::uint8_t* data, std::size_t size, bool copy = true)`: Wraps a raw buffer. With `copy = true` (default) it copies the bytes; with `copy = false` it references the caller-owned buffer, which **must** outlive the `DataSet`.
- `attach_to_memory(const std::string& name, const std::uint8_t* data, std::size_t size, bool copy = true)`: Same as above, but the stream identifier becomes `name` (helpful for diagnostics).
- `attach_to_memory(std::string name, std::vector<std::uint8_t>&& buffer)`: Moves an owning buffer into the dataset; the identifier is `name`.
- `path() const`: Returns the current stream identifier (file path, provided name, or `<memory>`).

Note: These attachment calls are intended for the root `DataSet` (the object returned by `read_file`/`read_bytes` or constructed directly). Sub-datasets created internally reuse the parent stream and should not reattach.

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
:members: DataElement, tag, vr, length, offset, value_span, as_sequence, as_pixel_sequence, set_tag, set_vr, set_length, set_offset, set_data, set_sequence, set_pixel_sequence
:undoc-members:
:::

:::{doxygenclass} dicom::DataSet
:project: dicomsdl
:members: DataSet, attach_to_file, attach_to_memory, path, add_dataelement, remove_dataelement, get_dataelement, dump_elements, read_attached_stream, ensure_loaded, is_little_endian, is_explicit_vr, transfer_syntax_uid, begin, end, cbegin
:undoc-members:
:::
