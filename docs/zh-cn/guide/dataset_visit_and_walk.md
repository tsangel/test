# DataSet Visit and Walk

`DataSet.visit()` 和 `DicomFile.visit()` 是 C++ 里的 callback-style fast
path，用来按 depth-first preorder 遍历 root dataset 以及所有 nested
sequence item dataset。

`DataSet.walk()` 和 `DicomFile.walk()` 则提供同一棵树的 iterator-style
遍历。

visit/walk 都会先访问 `SQ` data element 本身，再继续进入它下面的 item。
每个 step 会给出两部分信息：

- ancestors-only path view
- current `DataElement`

这很适合做 nested metadata inspection、按需跳过部分内容，以及像 UID
rewrite 这样的 transform-style pass。

`visit()` 和 `walk()` 都只遍历已经加载好的 dataset 状态，不会隐式调用
`ensure_loaded()` 或 `ensure_dataelement()`。

对于 partial-load 的 attached dataset，后面的 tag 会在 traversal 中被静默
漏掉。如果你的 pass 必须检查完整 dataset，请先 fully load，或者在
visit/walk 之前用 `ensure_loaded(tag)` 把需要的 frontier 读进来。

## 会访问什么

访问顺序如下：

1. current `DataElement`
2. 如果该 element 是 `SQ`，按顺序访问 nested item dataset
3. 再访问剩余的同级 elements

也就是说，`SQ` element 会先于它的子项被访问，所以这里天然就是一个
适合决定是否往下 skip 的位置。

## C++ visit

在 C++ 里，如果你不需要 iterator object，通常先看 `visit()` 更自然。

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

如果 callback 返回 `void`，会自动按
`DataSetVisitControl::continue_` 处理。

这些 control return 值都发生在 current element 已经传给 callback 之后。
它们不会撤销当前 step，只会改变后续还要不要继续向下或向后遍历。

- `DataSetVisitControl::skip_sequence`
  - 会在 current `SQ` element 的 callback 结束后生效
  - 会跳过它下面的 nested item dataset
  - 会从该 sequence 后面的下一个 sibling element 继续
- `DataSetVisitControl::skip_current_dataset`
  - 会在 current element 的 callback 结束后生效
  - 会跳过 current dataset 里剩余的所有 element
  - 在 root level 会结束整个 visit
  - 在 nested item dataset 中会从下一个 sibling item 或 parent sibling
    element 继续
- `DataSetVisitControl::stop`
  - 会立即停止整个 visit

### `DicomFile::visit(...)`

`DicomFile::visit(...)` 会转发到 root dataset：

```cpp
auto file = dicom::read_file("sample.dcm");

file->visit([](auto path, auto& element) {
  std::cout << path.to_string() << " -> " << element.tag().to_string() << "\n";
});
```

## C++ walk

如果你需要 `DataSetWalker`、iterator-style 代码，或者 yielded entry 上的
`skip_sequence()` 这类 live method，就用 `walk()`。

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

如果你想显式控制 iterator，iterator 侧也提供同样的 walk-control API。

```cpp
auto walker = ds.walk();
for (auto it = walker.begin(); it != walker.end(); ++it) {
  if (it->element.tag() == "PerFrameFunctionalGroupsSequence"_tag) {
    it->skip_sequence();
  }
}
```

这些 live control 的含义也是一样的：

- `entry.skip_sequence()` / `it->skip_sequence()`
  - current `SQ` element 本身仍然保留在 traversal 里
  - 会跳过该 sequence 下的所有 nested item dataset
  - 会继续到该 sequence 后面的下一个 sibling element
- `entry.skip_current_dataset()` / `it->skip_current_dataset()`
  - current element 本身仍然保留在 traversal 里
  - 会跳过 current dataset 里剩余的 element
  - 在 root level 会结束 walk
  - 在 nested item dataset 中会继续到下一个 sibling item 或 parent
    sibling element

## Python walk

Python 目前公开的是 `walk()`，不是 `visit()`。

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

`path` 是 ancestors-only view，不包含 current leaf tag。

例如：

- current location: `ReferencedSeriesSequence[0].SeriesInstanceUID`
- `path.to_string()`: `00081115.0`
- current `element.tag()`: `SeriesInstanceUID`

字符串格式使用 packed uppercase hex tag 和 dotted item index，因此和
dump/path 输出对照起来比较方便。

## Borrowed path 生命周期

`path` 是绑定到 current callback/step 的 borrowed view。

- 只在当前 callback 或 iteration step 内使用。
- 如果后面还要保存，存 `path.to_string()` 的结果。
- callback 结束或 walker advance 之后，不要继续长期持有 path object 本身。

这和 DicomSDL 里其他 borrowed view 的使用感觉一致。

## Walk control

有两个 walk-control 操作：

这些控制都发生在 current element 已经交给 caller 之后。它们不会移除
当前 step 本身，只会裁剪接下来还要遍历的部分。

- `skip_sequence()`
  - 只有 current entry 是 `SQ` element 时才有意义
  - 会在当前 `SQ` element 已经 yield 给 caller 后生效
  - 在当前 walk 中跳过该 sequence subtree
- `skip_current_dataset()`
  - 会在当前 element 已经 yield 给 caller 后生效
  - 在当前 walk 中跳过 current dataset 剩余的全部内容
  - 在 root level 会直接结束 walk
  - 在 nested item dataset 中会继续到下一个 sibling item 或 parent
    sibling element

这些操作在下面这些位置都可用：

- `DataSetWalkEntry`
- `DataSetWalkIterator`
- Python `DataSetWalkEntry`
- Python `DataSetWalkIterator`

对于 C++ `visit()`，对应的控制方式是：

- `return DataSetVisitControl::skip_sequence;`
- `return DataSetVisitControl::skip_current_dataset;`
- `return DataSetVisitControl::stop;`

## Related pages

- [Python DataSet Guide](python_dataset_guide.md)
- [C++ DataSet Guide](cpp_dataset_guide.md)
- [Sequence and Paths](sequence_and_paths.md)
