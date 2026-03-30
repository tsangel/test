# Selected Read

`read_file_selected(...)` and `read_bytes_selected(...)` read only the tags and
nested sequence children you select from a DICOM stream.

Use selected read when you want a small subset of top-level tags plus specific
nested sequence children, without loading the rest of the dataset.

## Selection tree

`DataSetSelection` is a nested tree.

- A leaf node selects only its tag.
- A nested node selects its tag and applies its children to every item dataset
  under that sequence.
- Private and unknown tags are allowed, including explicit tag strings such as `"70531000"`.

Construction canonicalizes the tree:

- `TransferSyntaxUID (0002,0010)` and `SpecificCharacterSet (0008,0005)` are
  injected at the root when absent.
- sibling tags are sorted in ascending tag order
- duplicate nodes are merged

## C++ example

```cpp
#include <dicom.h>
using namespace dicom::literals;

dicom::DataSetSelection selection{
    "StudyInstanceUID"_tag,
    "SeriesInstanceUID"_tag,
    {"ReferencedSeriesSequence"_tag, {
        "SeriesInstanceUID"_tag,
        "ReferencedSOPInstanceUID"_tag,
    }},
    "SOPInstanceUID"_tag,
};

auto file = dicom::read_file_selected("sample.dcm", selection);
auto& ds = file->dataset();

auto study = ds["StudyInstanceUID"_tag].to_uid_string();
auto& seq = ds["ReferencedSeriesSequence"_tag];
```

## Python reusable example

```python
import dicomsdl as dicom

selection = dicom.DataSetSelection(
    [
        "StudyInstanceUID",
        "SeriesInstanceUID",
        ("ReferencedSeriesSequence", [
            "SeriesInstanceUID",
            "ReferencedSOPInstanceUID",
        ]),
        "SOPInstanceUID",
    ]
)

df = dicom.read_file_selected("sample.dcm", selection)
```

## Python one-shot example

For one-shot reads, Python also accepts a raw nested selection tree directly:

```python
import dicomsdl as dicom

df = dicom.read_bytes_selected(
    data,
    [
        "SOPInstanceUID",
        ("ReferencedStudySequence", [
            "ReferencedSOPInstanceUID",
        ]),
    ],
    name="sample",
)
```

Use `DataSetSelection(...)` when you want to validate/canonicalize once and
reuse the same selection across many files.

## Semantics

- The returned `DicomFile` keeps only the selected tags and nested sequence
  children. Unselected tags behave as if they were missing.
- Root-level `TransferSyntaxUID` and `SpecificCharacterSet` are always
  considered, even when you do not list them explicitly.
- `ReadOptions.load_until` is ignored for selected-read APIs. The selected-read
  parser derives its own frontier from the selection.
- Selecting only an `SQ` element keeps that sequence present when it exists in
  the source. Item datasets stay present, but their selected child set may be
  empty.
- Private and unknown tags are valid selection targets, including explicit tag strings such as `"70531000"`.
- `keep_on_error` behaves like the ordinary read APIs, but only for the region
  that selected read actually visits.
- Malformed data outside the selected region may remain unseen and therefore
  may not set `has_error` or `error_message`.

## Related docs

- [File IO](file_io.md)
- [DataSet Visit and Walk](dataset_visit_and_walk.md)
- [C++ API Overview](../reference/cpp_api.md)
- [DicomFile Reference](../reference/dicomfile_reference.md)
- [Python API Reference](../reference/python_reference.md)
