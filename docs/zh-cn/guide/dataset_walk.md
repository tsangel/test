# DataSet Walk

`DataSet.walk()` 和 `DicomFile.walk()` 会按 depth-first preorder 遍历
root dataset 以及所有 nested sequence item dataset。

walk 会先访问 `SQ` data element 本身，再继续进入它下面的 item。
每个 step 会给出两部分信息：

- ancestors-only path view
- current `DataElement`

这很适合做 nested metadata inspection、selective pruning，以及像 UID
rewrite 这样的 transform-style pass。

`walk()` 只遍历已经加载好的 dataset 状态，不会隐式调用
`ensure_loaded()` 或 `ensure_dataelement()`。

对于 partial-load 的 attached dataset，后面的 tag 会在 walk 中被静默漏掉。
如果你的 pass 必须检查完整 dataset，请先 fully load，或者在 walk 之前用
`ensure_loaded(tag)` 把需要的 frontier 读进来。

## 会访问什么

访问顺序如下：

1. current `DataElement`
2. 如果该 element 是 `SQ`，按顺序访问 nested item dataset
3. 再访问剩余的同级 elements

也就是说，`SQ` element 本身对调用方是可见的，所以可以在这个位置做
walk control。

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

如果你想显式控制 iterator，iterator 侧也提供同样的 walk-control API。

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

如果你只想直接拿到 path 和 element，也可以 unpack：

```python
for path, elem in df.walk():
    print(path.to_string(), elem.tag)
```

不过如果需要控制遍历流程，通常 `for entry in df.walk():` 更清晰，因为
`skip_sequence()` 和 `skip_current_dataset()` 同时挂在 entry 和 walker 上。

## Path 语义

`entry.path` 是 ancestors-only view，不包含 current leaf tag。

例如：

- current location: `ReferencedSeriesSequence[0].SeriesInstanceUID`
- `entry.path.to_string()`: `00081115.0`
- `entry.element.tag()`: `SeriesInstanceUID`

字符串格式使用 packed uppercase hex tag 和 dotted item index，因此和
dump/path 输出对照起来比较方便。

## Borrowed path 生命周期

`entry.path` 是绑定到 current walk step 的 borrowed view。

- 只在当前 iteration step 内使用。
- 如果后面还要保存，存 `entry.path.to_string()` 的结果。
- walker advance 之后，不要继续长期持有 path object 本身。

这和 DicomSDL 里其他 borrowed view 的使用感觉一致。

## Walk control

有两个 walk-control 操作：

- `skip_sequence()`
  - 只有 current entry 是 `SQ` element 时才有意义
  - 在当前 walk 中跳过该 sequence subtree
- `skip_current_dataset()`
  - 在当前 walk 中跳过 current dataset 剩余的全部内容
  - 在 root level 会直接结束 walk
  - 在 nested item dataset 中会继续到下一个 sibling item 或 parent
    sibling element

这些操作在下面这些位置都可用：

- `DataSetWalkEntry`
- `DataSetWalkIterator`
- Python `DataSetWalkEntry`
- Python `DataSetWalkIterator`

## Related pages

- [Python DataSet Guide](python_dataset_guide.md)
- [C++ DataSet Guide](cpp_dataset_guide.md)
- [Sequence and Paths](sequence_and_paths.md)
