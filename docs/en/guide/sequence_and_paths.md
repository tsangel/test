# Sequence and Paths

Nested DICOM data is easiest to read and write through dotted tag paths such as `SequenceKeyword.0.LeafKeyword`.

## C++

```cpp
#include <dicom.h>

dicom::DataSet ds;
ds.ensure_dataelement("ReferencedStudySequence.0.ReferencedSOPInstanceUID", dicom::VR::UI)
  .from_uid_string("1.2.3");

const auto& uid =
    ds["ReferencedStudySequence.0.ReferencedSOPInstanceUID"];
```

## Python

```python
import dicomsdl as dicom

df = dicom.read_file("sample.dcm")
ds = df.dataset
half_life = ds.get_value("RadiopharmaceuticalInformationSequence.0.RadionuclideHalfLife")
ds.set_value("ReferencedStudySequence.0.ReferencedSOPInstanceUID", "1.2.3")
```

## Notes

- The path format is `SequenceName.item_index.LeafName`.
- Use this path form when you want one-shot nested lookup or assignment.
- In C++, `operator[]`, `get_dataelement(...)`, and `get_value(...)` all accept dotted tag paths.
- When `ensure_dataelement(...)` materializes a nested path under an existing non-sequence intermediate element, it can reset that intermediate element to `SQ`.
- If you need low-level traversal details, keep a reference to the returned `Sequence` / item dataset and inspect elements directly.

## Related docs

- [Core Objects](core_objects.md)
- [C++ DataSet Guide](cpp_dataset_guide.md)
- [Python DataSet Guide](python_dataset_guide.md)
- [Tag-path Lookup Semantics](../reference/tag_path_lookup.md)
