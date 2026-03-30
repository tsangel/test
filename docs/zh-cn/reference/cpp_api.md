# C++ API Overview

```{note}
本页正文目前仍为英文原文。需要时请以英文版为准。
```

C++ consumers only need `#include <dicom.h>` to access core types and the perfect-hash dictionary lookups. Add `using namespace dicom::literals;` to enable user-defined literals like `"PatientName"_tag`.

## Minimal example
```cpp
#include <dicom.h>
using namespace dicom::literals;

int main() {
  dicom::DataSet ds;
  ds.attach_to_file("sample.dcm");

  dicom::ReadOptions opts;
  opts.load_until = "Rows"_tag;
  ds.read_attached_stream(opts);

  const long rows = ds.get_value<long>("Rows"_tag, 0L);
  // use rows
}
```

For a higher-level guide focused on `DicomFile`, `DataSet`, and `DataElement` access patterns, see
[C++ DataSet Guide](../guide/cpp_dataset_guide.md).

## Preferred DataElement access pattern

- Use `dataset.get_value<T>(tag, default)` for one-shot typed reads.
- Use `dataset.get_value<std::string_view>(...)` or `dataset.get_value<std::vector<std::string_view>>(...)`
  when you want zero-copy text access and can honor the returned view lifetime.
- Use `dataset.set_value(tag, value)` for one-shot typed writes.
- Use `dataset[tag_or_path].to_xxx().value_or(default)` when you are already working with a `DataElement`.
- Use `if (auto& e = dataset[tag]; e)` only when element presence itself matters.
- Use `get_dataelement(...)` when you want the same lookup as `dataset[tag_or_path]` but in named-method form.
  It returns `DataElement&` and yields the same `VR::None` element that evaluates to `false` on miss.
- `get_value(...)` does not implicitly continue partial loading; data elements that have not been parsed yet still behave as missing.
- `set_value(...)` also does not implicitly continue partial loading; operations on data elements that have not been parsed yet raise.
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
ok &= ds.set_value("Rows"_tag, 512L);
ok &= ds.set_value("Columns"_tag, -1L);
ok &= ds.set_value("BitsAllocated"_tag, 16L);

if (!ok) {
  auto messages = reporter->take_messages();
  // inspect or surface messages
}

dicom::diag::set_thread_reporter(nullptr);
```

On a partially loaded attached dataset, `set_value(...)`, `add_dataelement(...)`, and
`ensure_dataelement(...)` all throw when the target data element has not been parsed yet instead of
mutating unread tail data.

Note: `add_dataelement(...)` returns `DataElement&` and can still throw on validation/allocation errors.

For chaining-friendly "ensure presence" code, `ensure_dataelement(...)` is often a better fit:

```cpp
auto& rows = ds.ensure_dataelement("Rows"_tag);
auto& private_elem = ds.ensure_dataelement(dicom::Tag(0x0009, 0x0030), dicom::VR::US);
auto& nested =
    ds.ensure_dataelement("ReferencedStudySequence.0.ReferencedSOPInstanceUID", dicom::VR::UI);
bool nested_ok =
    ds.set_value("ReferencedStudySequence.0.ReferencedSOPInstanceUID", std::string_view("1.2.3"));
```

- If the element already exists and `vr` is omitted (default `VR::None`) or explicitly `VR::None`, the existing element is returned unchanged.
- If the element already exists and `vr` is explicit and different, the existing element is reset
  in place so the requested VR is guaranteed.
- If the element is missing and `vr` is explicit, a new zero-length element is inserted with that VR.
- If the element is missing and `vr` is omitted, standard tags resolve their dictionary VR, while
  unknown/private tags throw.
- Dotted tag-path forms create intermediate sequence elements/items as needed. When
  `ensure_dataelement(...)` has to materialize a nested path under an existing non-sequence
  intermediate element, it can reset that element to `SQ`.
- `add_dataelement(...)` remains the always-replace API; `ensure_dataelement(...)` only resets
  when an explicit VR must be enforced.
- On partially loaded attached datasets, calling `ensure_dataelement(...)` for a tag whose data
  element has not been parsed yet throws instead of implicitly continuing the load.

See also the runnable examples:

- `examples/dataset_access_example.cpp`
- `examples/batch_assign_with_error_check.cpp`

## DataSet attachment methods

- `attach_to_file(const std::filesystem::path& path)`: Opens `path` via `InFileStream`, sets the stream identifier to the path, and attaches the source. Call `read_attached_stream(...)` to perform the initial parse; after a partial read, use `ensure_loaded(tag)` to advance farther without reparsing from the beginning.
- `attach_to_memory(const std::uint8_t* data, std::size_t size, bool copy = true)`: Wraps a raw buffer. With `copy = true` (default) it copies the bytes; with `copy = false` it references the caller-owned buffer, which **must** outlive the `DataSet`.
- `attach_to_memory(const std::string& name, const std::uint8_t* data, std::size_t size, bool copy = true)`: Same as above, but the stream identifier becomes `name` (helpful for diagnostics).
- `attach_to_memory(std::string name, std::vector<std::uint8_t>&& buffer)`: Moves an owning buffer into the dataset; the identifier is `name`.
- `path() const`: Returns the current stream identifier (file path, provided name, or `<memory>`).

Note: These attachment calls are intended for the root `DataSet` (the object returned by `read_file`/`read_bytes` or constructed directly). Sub-datasets created internally reuse the parent stream and should not reattach.

## Selected read

- `DataSetSelectionNode` 是一个嵌套 selection node。`children` 为空时只选择
  tag 本身；`children` 非空时，会把 child selection 应用到该 sequence 下所有
  item dataset。
- `DataSetSelection` 在构造时会 canonicalize。它会在 root level 缺失时自动加入
  `TransferSyntaxUID (0002,0010)` 和 `SpecificCharacterSet (0008,0005)`，
  对 sibling tag 按升序排序，并合并重复节点。
- private tag 和 unknown tag 也允许出现在 selection tree 中，也可以写成 `"70531000"` 这样的 explicit tag string。
- `read_file_selected(...)` 和 `read_bytes_selected(...)` 会返回一个只保留所选
  tag 和嵌套 sequence child 的 `DicomFile`。
- `ReadOptions.keep_on_error` 和 memory-buffer `ReadOptions.copy` 仍然有效。
  `ReadOptions.load_until` 在 selected-read API 中会被忽略。
- 即使只选择 `SQ` 本身，也会保留 source 中存在的 sequence 和 item count。
- 选择/访问区域之外的 malformed data 可能根本不会被看到，因此也可能不会反映到
  `has_error` 或 `error_message` 中。
- 更完整的例子请参见 [Selected Read](../guide/selected_read.md)。

## DataSet visiting and walking

- `DataSet::visit(fn)` and `DicomFile::visit(fn)` provide the C++ callback-style fast path for depth-first preorder traversal over the root dataset and all nested sequence item datasets.
- The `visit(...)` callback receives `(DataSetVisitPathRef path, DataElement& element)` or `(DataSetVisitPathRef path, const DataElement& element)` and may return `DataSetVisitControl`.
- `DataSetVisitControl::skip_sequence`, `DataSetVisitControl::skip_current_dataset`, and `DataSetVisitControl::stop` provide visit-time skip and early-stop control.
- `DataSet::walk()` and `DicomFile::walk()` return a `DataSetWalker` for the same traversal order when you want an iterator-style API.
- Each walk step yields `DataSetWalkEntry { path, element }`.
- `path` is an ancestors-only borrowed view in both APIs. Persist `path.to_string()` if you need to keep it after the current callback/iterator step.
- Traversal only covers the dataset state that is already loaded; it does not implicitly call `ensure_loaded()` or `ensure_dataelement()`.
- On partially loaded attached datasets, tags beyond the loaded frontier are silently absent from visit/walk. Fully load first or call `ensure_loaded(tag)` before using traversal for full-dataset passes.
- `DataSetWalkEntry::skip_sequence()` / `DataSetWalkIterator::skip_sequence()` skip the current sequence subtree.
- `DataSetWalkEntry::skip_current_dataset()` / `DataSetWalkIterator::skip_current_dataset()` skip the rest of the current dataset.
- Full usage examples in C++ and Python: [DataSet Visit and Walk](../guide/dataset_visit_and_walk.md)

## UID generation helpers

- `dicom::uid::generate_uid()`: create a base generated UID.
- `dicom::uid::generate_uid_from(key, root)`: create a deterministic UID from a stable key and root.
- `dicom::uid::Generated::append(component)`: append one UID component with fallback policy.
- Full behavior and Python equivalents: [Generating UID](../guide/generating_uid.md)

## UID remapping

- `dicom::UidRemapper::in_memory(journal_path, uid_root, flush_on_each_insert)`: open a journal-backed single-process remapper.
- `map_uid(source_uid)`: reuse an existing persisted mapping or create and persist a new one.
- `RewriteUidOptions`: choose which UID categories to rewrite. Study/Series/SOP UIDs are enabled by default; Frame of Reference UIDs are opt-in.
- `rewrite_uids(dataset_or_file, remapper, options)`: rewrite selected already-loaded UI elements with `DataSet::visit(...)` and return the rewritten element count.
- `close()`: flush pending journal state and release the single-writer lock explicitly.
- `flush_on_each_insert = true` is the safer default. `false` can improve miss-path performance, but callers should catch exceptions and call `close()` during orderly shutdown.
- `UidRemapper` is currently a C++-only API. There is no Python binding yet.

## Pixel transform metadata

Frame-aware pixel transform metadata resolution for `rescale_transform(frame)`,
`window_transform(frame)`, `voi_lut(frame)`, and `modality_lut(frame)` is documented in
[Pixel Transform Metadata Resolution](pixel_transform_metadata.md).

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
:members: DataSet, attach_to_file, attach_to_memory, path, add_dataelement, remove_dataelement, operator[], get_dataelement, set_value, dump_elements, read_attached_stream, ensure_loaded, is_explicit_vr, transfer_syntax_uid, begin, end, cbegin
:undoc-members:
:::
