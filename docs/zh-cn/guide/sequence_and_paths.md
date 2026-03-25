# 序列和路径

嵌套 DICOM 数据最容易通过点分标签路径（例如 `SequenceKeyword.0.LeafKeyword`）读写。

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

## 说明

- 路径格式为`SequenceName.item_index.LeafName`。
- 当您想一次完成嵌套查找或赋值时，请使用这种路径写法。
- 在 C++ 中，`operator[]`、`get_dataelement(...)` 和 `get_value(...)` 都接受点分标签路径。
- 当 `ensure_dataelement(...)` 在现有的非序列中间元素下实际创建嵌套路径时，它可能会把该中间元素重置为 `SQ`。
- 如果您需要更底层的遍历细节，请保留返回的 `Sequence` / item 数据集引用，并直接检查其中的元素。

## 相关文档

- [核心对象](core_objects.md)
- [C++ 数据集指南](cpp_dataset_guide.md)
- [Python数据集指南](python_dataset_guide.md)
- [标签路径查找语义](../reference/tag_path_lookup.md)
