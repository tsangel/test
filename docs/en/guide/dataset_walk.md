# DataSet Walk

`DataSet.walk()` and `DicomFile.walk()` provide a depth-first preorder traversal
over the root dataset and all nested sequence item datasets.

The walk includes `SQ` data elements themselves before descending into their
items. Each step yields:

- a borrowed ancestors-only path view
- the current `DataElement`

This is useful for nested metadata inspection, selective pruning, and
transform-style passes such as UID rewriting.

`walk()` only traverses the dataset state that is already loaded. It does not
implicitly call `ensure_loaded()` or `ensure_dataelement()`.

On a partially loaded attached dataset, later tags are silently absent from the
walk. If your pass must inspect the full dataset, fully load it first or call
`ensure_loaded(tag)` for the frontier you need before walking.

## What gets visited

Walk order is:

1. the current `DataElement`
2. if that element is `SQ`, each nested item dataset in order
3. the remaining sibling elements

That means `SQ` elements are visible to the caller and can be used as pruning
points.

## C++ example

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

## Python example

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

If you need pruning, `for entry in df.walk():` is usually the clearer style,
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

- `skip_sequence()`
  - valid when the current entry is an `SQ` element
  - skips that sequence subtree during the current walk
- `skip_current_dataset()`
  - skips the rest of the current dataset during the current walk
  - at root level, this ends the walk
  - inside a nested item dataset, this continues at the next sibling item or
    parent sibling element

These operations are available on:

- `DataSetWalkEntry`
- `DataSetWalkIterator`
- Python `DataSetWalkEntry`
- Python `DataSetWalkIterator`

## Related pages

- [Python DataSet Guide](python_dataset_guide.md)
- [C++ DataSet Guide](cpp_dataset_guide.md)
- [Sequence and Paths](sequence_and_paths.md)
