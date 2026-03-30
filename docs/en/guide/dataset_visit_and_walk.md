# DataSet Visit and Walk

`DataSet.visit()` and `DicomFile.visit()` provide the C++ callback-style fast
path for depth-first preorder traversal over the root dataset and all nested
sequence item datasets.

`DataSet.walk()` and `DicomFile.walk()` provide the iterator-style traversal for
the same tree.

Both visit/walk forms include `SQ` data elements themselves before descending
into their
items. Each step yields:

- a borrowed ancestors-only path view
- the current `DataElement`

This is useful for nested metadata inspection, selective skipping, and
transform-style passes such as UID rewriting.

Neither `visit()` nor `walk()` traverses beyond the dataset state that is
already loaded. They do not
implicitly call `ensure_loaded()` or `ensure_dataelement()`.

On a partially loaded attached dataset, later tags are silently absent from the
traversal. If your pass must inspect the full dataset, fully load it first or
call `ensure_loaded(tag)` for the frontier you need before visiting/walking.

## What gets visited

Walk order is:

1. the current `DataElement`
2. if that element is `SQ`, each nested item dataset in order
3. the remaining sibling elements

That means `SQ` elements are visited before their children, so they are natural
skip points.

## C++ visit

For C++ code, start with `visit()` when you do not need an iterator object.

### `DataSet::visit(...)`

```cpp
#include <dicom.h>
#include <iostream>
using namespace dicom::literals;

auto file = dicom::read_file("sample.dcm");
auto& ds = file->dataset();

ds.visit([](auto path, auto& element) {
  if (element.tag() == "PerFrameFunctionalGroupsSequence"_tag) {
    return dicom::DataSetVisitControl::skip_sequence;
  }
  std::cout << path.to_string() << " -> " << element.tag().to_string() << "\n";
  return dicom::DataSetVisitControl::continue_;
});
```

A void-returning callback is treated as
`DataSetVisitControl::continue_`.

The control return values apply after the current element has already been
visited. They do not undo the current callback step.

- `DataSetVisitControl::skip_sequence`
  - applies after the callback for the current `SQ` element
  - skips its nested item datasets
  - resumes at the next sibling element after that sequence
- `DataSetVisitControl::skip_current_dataset`
  - applies after the callback for the current element
  - skips the remaining elements in the current dataset
  - at root level, ends the whole visit
  - inside a nested item dataset, resumes at the next sibling item or parent
    sibling element
- `DataSetVisitControl::stop`
  - stops the whole visit immediately

### `DicomFile::visit(...)`

`DicomFile::visit(...)` forwards to the root dataset:

```cpp
auto file = dicom::read_file("sample.dcm");

file->visit([](auto path, auto& element) {
  std::cout << path.to_string() << " -> " << element.tag().to_string() << "\n";
});
```

## C++ walk

Use `walk()` when you want a `DataSetWalker`, iterator-style code, or live
entry methods such as `skip_sequence()` on the yielded entry.

### `DataSet::walk(...)`

```cpp
#include <dicom.h>
#include <iostream>
using namespace dicom::literals;

auto file = dicom::read_file("sample.dcm");
auto& ds = file->dataset();

for (auto entry : ds.walk()) {
  const auto path = entry.path.to_string();
  const auto tag = entry.element.tag();

  std::cout << path << " -> " << tag.to_string() << "\n";

  if (tag == "PerFrameFunctionalGroupsSequence"_tag) {
    entry.skip_sequence();
  }
}
```

If you prefer explicit traversal control, the iterator exposes the same
walk-control operations:

```cpp
auto walker = ds.walk();
for (auto it = walker.begin(); it != walker.end(); ++it) {
  if (it->element.tag() == "PerFrameFunctionalGroupsSequence"_tag) {
    it->skip_sequence();
  }
}
```

These live controls have the same skipping behavior:

- `entry.skip_sequence()` / `it->skip_sequence()`
  - keeps the current `SQ` element in the traversal
  - skips all nested item datasets under that sequence
  - continues at the next sibling element after that sequence
- `entry.skip_current_dataset()` / `it->skip_current_dataset()`
  - keeps the current element in the traversal
  - skips the remaining elements in the current dataset
  - at root level, ends the walk
  - inside a nested item dataset, continues at the next sibling item or parent
    sibling element

## Python walk

Python currently exposes `walk()`, not `visit()`.

```python
import dicomsdl as dicom

df = dicom.read_file("sample.dcm")

for entry in df.walk():
    print(entry.path.to_string(), entry.element.tag)
    if entry.element.tag == dicom.Tag("PerFrameFunctionalGroupsSequence"):
        entry.skip_sequence()
```

`walk()` also supports unpacking when you only want the path and element:

```python
for path, elem in df.walk():
    print(path.to_string(), elem.tag)
```

If you need to skip nested parts of the traversal, `for entry in df.walk():`
is usually the clearer style,
because `skip_sequence()` and `skip_current_dataset()` live on the entry and the
walker.

## Path semantics

`entry.path` is an ancestors-only view. It does not include the current leaf
tag.

Example:

- current location: `ReferencedSeriesSequence[0].SeriesInstanceUID`
- `entry.path.to_string()`: `00081115.0`
- `entry.element.tag()`: `SeriesInstanceUID`

The string form uses packed uppercase hex tags plus dotted item indexes so it
is easy to compare with dump/path output.

## Borrowed path lifetime

`entry.path` is a borrowed view tied to the current walk step.

- Use it inside the current iteration step.
- If you need to keep it, store `entry.path.to_string()`.
- Do not keep the path object itself after the walker advances.

This matches the usual borrowed-view style used elsewhere in DicomSDL.

## Walk control

Two walk-control operations are available.

They apply after the current element has already been yielded to the caller.
They do not remove the current step itself; they only skip what comes next.

- `skip_sequence()`
  - valid when the current entry is an `SQ` element
  - applies after yielding the current `SQ` element
  - skips that sequence subtree during the current walk
- `skip_current_dataset()`
  - applies after yielding the current element
  - skips the rest of the current dataset during the current walk
  - at root level, this ends the walk
  - inside a nested item dataset, this continues at the next sibling item or
    parent sibling element

These operations are available on:

- `DataSetWalkEntry`
- `DataSetWalkIterator`
- Python `DataSetWalkEntry`
- Python `DataSetWalkIterator`

For C++ `visit()`, the equivalent controls are:

- `return DataSetVisitControl::skip_sequence;`
- `return DataSetVisitControl::skip_current_dataset;`
- `return DataSetVisitControl::stop;`

## Related pages

- [Python DataSet Guide](python_dataset_guide.md)
- [C++ DataSet Guide](cpp_dataset_guide.md)
- [Sequence and Paths](sequence_and_paths.md)
