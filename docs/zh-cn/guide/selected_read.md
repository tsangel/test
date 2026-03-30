# Selected Read

`read_file_selected(...)` 和 `read_bytes_selected(...)` 会从 DICOM 流里只读取
你选中的 tag 和嵌套 sequence child。

当你只需要少量顶层 tag，以及嵌套 sequence 里的特定 child，而不想把其余
dataset 全部 materialize 出来时，可以使用 selected read。

## Selection tree

`DataSetSelection` 是一个嵌套树。

- leaf node 只选择它自己的 tag。
- nested node 会选择它自己的 tag，并把 child selection 应用到该 sequence
  下所有 item dataset。
- private tag 和 unknown tag 也允许使用，也可以写成 `"70531000"` 这样的 explicit tag string。

构造时会自动完成下面这些规范化步骤。

- 如果 root level 没有显式给出，就自动加入
  `TransferSyntaxUID (0002,0010)` 和 `SpecificCharacterSet (0008,0005)`
- sibling tag 按升序排序
- 合并重复 node

## C++ 示例

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
```

## Python 可复用示例

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

## Python one-shot 示例

在 Python 里，one-shot 调用也可以直接传 raw nested selection tree。

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

如果你打算在很多文件上复用同一个 selection，更推荐先构造一次
`DataSetSelection(...)`。

## Semantics

- 返回的 `DicomFile` 里只包含选中的 tag 和嵌套 sequence child。
  未选中的 tag 会表现得像不存在一样。
- root level 的 `TransferSyntaxUID` 和 `SpecificCharacterSet` 即使没有显式写进
  selection，也始终会被考虑。
- 在 selected-read API 中，`ReadOptions.load_until` 会被忽略。
  读取 frontier 会根据 selection 在内部推导。
- 即使只选择了 `SQ` 本身，只要 source 里存在该 sequence，返回结果里也会保留
  这个 present 的 `SQ`。item dataset 也会保留，但如果没有选中任何 child，
  item dataset 可能是空的。
- private tag 和 unknown tag 都可以作为 selection 目标，也可以写成 `"70531000"` 这样的 explicit tag string。
- `keep_on_error` 的行为与普通 read 类似，但只作用于 selected read
  实际访问到的区域。
- 选择区域之外的 malformed data 可能完全不会被看到，因此也可能不会体现在
  `has_error` 或 `error_message` 中。

## 相关文档

- [File IO](file_io.md)
- [DataSet Visit and Walk](dataset_visit_and_walk.md)
- [C++ API Overview](../reference/cpp_api.md)
- [DicomFile Reference](../reference/dicomfile_reference.md)
- [Python API Reference](../reference/python_reference.md)
