# C++ 数据集指南

这是 `dicomsdl` 中面向用户的主要 C++ 数据集指南。
它介绍主要对象之间的关系、标签的写法，以及最重要的读写模式。

## dicomsdl 如何映射到 DICOM

`dicomsdl` 公开了一小组相关的 C++ 对象：

- `DicomFile`：拥有根数据集的文件/会话包装器
- `DataSet`：DICOM `DataElement` 对象的容器
- `DataElement`：一个 DICOM 字段，带有标签 / VR / 长度元数据，以及类型化 / 原始值访问
- `Sequence`：`SQ` 值的嵌套项目容器
- `PixelSequence`：用​​于封装或压缩像素数据的帧/片段容器

有关对象模型和 DICOM 映射，请参阅[核心对象](core_objects.md)。

C++ 中没有 Python 风格的属性语法糖。
标签、关键字和点分标签路径保持明确。

### DicomFile 和数据集

大多数数据元素访问 API 都是在 `DataSet` 上实现的。
`DicomFile` 拥有根 `DataSet` 并处理面向文件的工作，例如加载、保存和转码。
为了方便起见，`DicomFile` 转发了许多根数据集帮助程序，例如 `get_value(...)`、
`get_dataelement(...)`、`set_value(...)`、`ensure_dataelement(...)` 和 `ensure_loaded(...)`。

对于一些与文件级操作混合的根级读取，`DicomFile` 转发通常就足够了：

```cpp
#include <dicom.h>
using namespace dicom::literals;

auto file = dicom::read_file("sample.dcm");

long rows1 = file->get_value<long>("Rows"_tag, 0L);
const auto& patient_name1 = file->get_dataelement("PatientName"_tag);
```

对于重复的数据集工作，显式采用 `DataSet` 通常会更清晰：

```cpp
#include <dicom.h>
using namespace dicom::literals;

auto file = dicom::read_file("sample.dcm");
auto& ds = file->dataset();

long rows2 = ds.get_value<long>("Rows"_tag, 0L);
const auto& patient_name2 = ds["PatientName"_tag];
```

## 推荐 API

| API | 返回值 | 缺失时的行为 | 用途 |
| --- | --- | --- | --- |
| `ds.get_value<long>("Rows"_tag)` | `std::optional<long>` | `std::nullopt` |类型化读取，其中 `std::nullopt` 表示缺失 |
| `ds.get_value<long>("Rows"_tag, 0L)` | `long` | 返回传入的默认值 | 一次完成的类型化读取 |
| `ds["Rows"_tag]`、`ds["Rows"]`、`ds.get_dataelement("Rows")` | `DataElement&` |返回 `VR::None` 且会被判定为 `false` 的元素 |类型化读取 + 元数据访问 |
| `if (const auto& e = ds["Rows"_tag]; e)` | 根据是否存在分支 | 未命中时为 `false` | 需要区分元素是否存在的代码 |
| `ds.ensure_loaded("(0028,FFFF)"_tag)` | `void` | 无效使用时抛出异常 | 显式把部分读取继续推进到更后的标签边界 |
| `ds.ensure_dataelement("Rows"_tag, dicom::VR::US)` | `DataElement&` | 返回已有元素或插入新元素 | 便于链式调用的 ensure/create |
| `ds.set_value("Rows"_tag, 512L)` | `bool` | 编码/赋值失败时为 `false` | 一次完成的 ensure + 类型化赋值 |
| `ds.add_dataelement("Rows"_tag, dicom::VR::US)` | `DataElement&` |创建/替换 |显式叶插入 |

## 在 C++ 中拼写标签的方法

用户定义的文字后缀是小写的 `"_tag"`，而不是 `"_Tag"`。

| 形式 | 示例 | 优先考虑的场景 |
| --- | --- | --- |
| 关键字字面量 | `"Rows"_tag` | 日常 C++ 代码里处理大多数标准标签时 |
| 数字标签字面量 | `"(0028,0010)"_tag` | 数字标签写法更清楚时 |
| 组/元素构造函数 | `dicom::Tag(0x0028, 0x0010)` | group 和 element 已经分别存在时 |
| 打包标签值构造函数 | `dicom::Tag(0x00280010)` | 标签已经是 packed `0xGGGGEEEE` 值时 |
| 运行时文本解析 | `dicom::Tag("Rows")`, `dicom::Tag("(0028,0010)")` | 关键字或数字标签在运行时以文本形式传入时 |
| 字符串/路径形式 | `ds["Rows"]`, `ds.get_value<double>("00540016.0.00181074")` | 想做关键字查找，或一步完成嵌套读写时 |

### `"Rows"_tag`

- 日常 C++ 代码里处理大多数标准标签时，优先用它。
- 优点: 简短、可读、编译期检查、没有运行时解析。
- 权衡: 只能用于编译期已知的字符串字面量。

### `"(0028,0010)"_tag`

- 当数字标签写法比关键字更直观时，用它。
- 优点: 没有歧义，能在编译期检查，也适用于仅接受 Tag 的 API。
- 权衡: 比关键字更冗长，也更容易输错。

### `dicom::Tag(0x0028, 0x0010)`

- 当 group 和 element 已经分别作为运行时值存在时，用它。
- 优点: 显式、适合运行时值、不需要文本解析。
- 权衡: 比关键字字面量更啰嗦。

### `dicom::Tag(0x00280010)`

- 当标签已经是 packed `0xGGGGEEEE` 值时，用它。
- 优点: 与生成表或 packed tag 值集成时更紧凑。
- 权衡: 不能把裸 `0x00280010` 直接传给 `DataSet` API；要先包成 `Tag(...)` 或 `Tag::from_value(...)`。

### `dicom::Tag("Rows")` 或 `dicom::Tag("(0028,0010)")`

- 当关键字或数字标签是在运行时以文本形式传入时，用它。
- 优点: 同时接受关键字文本和数字标签文本。
- 权衡: 运行时会做解析，文本无效时可能抛出异常。

### 字符串/路径形式

- 当你想做关键字查找、点分标签路径遍历，或一步完成嵌套读写时，用它。
- 示例: `ds["Rows"]`, `ds.get_value<double>("00540016.0.00181074")`, `ds.set_value("PatientName", "Doe^Jane")`, `ds.ensure_dataelement("ReferencedStudySequence.0.ReferencedSOPInstanceUID", dicom::VR::UI)`.
- 优点: 不需要显式构造 `Tag`，而且 `operator[]`、`get_dataelement(...)`、`get_value(...)`、`set_value(...)`、`ensure_dataelement(...)`、`add_dataelement(...)` 都支持嵌套路径。
- 权衡: 字符串解析发生在运行时。

实用建议：

- 在大多数 C++ 代码中使用 `"Rows"_tag` 进行常用标签访问
- 当数字标签拼写最清晰时使用 `"(0028,0010)"_tag`
- 当标签来自运行时整数或打包值时使用 `dicom::Tag(...)`
- 当您需要嵌套查找或一步写入时，请使用字符串/路径形式，例如 `ds.get_value<double>("RadiopharmaceuticalInformationSequence.0.RadionuclideTotalDose")`、`ds.get_value<double>("00540016.0.00181074")`、`ds.set_value("ReferencedStudySequence.0.ReferencedSOPInstanceUID", "1.2.3")` 或 `ds.ensure_dataelement("ReferencedStudySequence.0.ReferencedSOPInstanceUID", dicom::VR::UI)`

## 读取值

### 快速路径：get_value<T>()

当您只需要类型化的值时，请使用 `get_value<T>()`：

```cpp
#include <dicom.h>
using namespace dicom::literals;

auto file = dicom::read_file("sample.dcm");
auto& ds = file->dataset();

if (auto rows = ds.get_value<long>("Rows"_tag)) {
  // 使用*rows
}

double slope = ds.get_value<double>("RescaleSlope"_tag, 1.0);
auto desc = ds.get_value<std::string_view>("StudyDescription"_tag);
```

- `get_value<T>(tag)` 返回 `std::optional<T>`。当您希望在调用端区分“缺失”和“存在真实值”时，请使用它。
- `get_value<T>(tag, default_value)` 返回 `T`。当您需要内联的默认值，并且不需要区分空结果和默认值路径时，请使用它。
- 默认值重载实际上是 `get_value<T>(...).value_or(default_value)`。
- `get_value<std::string_view>(...)` 是零拷贝视图。在使用时保持拥有的数据集/文件处于活动状态。

### 数据元素访问：operator[]

当您想要 `DataElement` 而不仅仅是值时，请使用 `operator[]`：

```cpp
#include <dicom.h>
using namespace dicom::literals;

auto file = dicom::read_file("sample.dcm");
auto& ds = file->dataset();

const auto& rows_elem = ds["Rows"];
if (rows_elem) {
  long rows = rows_elem.to_long(0L);
}

const auto& dose_elem =
    ds["RadiopharmaceuticalInformationSequence.0.RadionuclideTotalDose"];
```

`operator[]` 接受 `Tag`、关键字字符串或点分标签路径。
当您想要编译时拼写而不需要运行时解析时，请使用 `"_tag"`。

### 存在检查

当存在本身很重要时，对返回的 `DataElement` 做真值判定：

```cpp
if (const auto& rows_elem = ds["Rows"_tag]; rows_elem) {
  long rows = rows_elem.to_long(0L);
}

if (const auto& patient_name = ds.get_dataelement("PatientName"); patient_name) {
  // 存在的元素
}
```

缺失查找会返回一个带有 `VR::None` 且会被判定为 `false` 的元素，而不是抛出异常。

### 方法形式的相同查找：get_dataelement(...)

`get_dataelement(...)` 执行与 `operator[]` 相同的查找。当命名函数读起来比 `ds[...]` 更清晰时，某些代码库更喜欢方法拼写：

```cpp
const auto& dose = ds.get_dataelement(
    "RadiopharmaceuticalInformationSequence.0.RadionuclideTotalDose");
if (dose) {
  double value = dose.to_double(0.0);
}
```

### 继续部分加载

当部分读取在您现在需要的标签之前停止时，请使用 `ensure_loaded(tag)`：

```cpp
ds.ensure_loaded("(0028,FFFF)"_tag);

long rows = ds.get_value<long>("Rows"_tag, 0L);
long cols = ds.get_value<long>("Columns"_tag, 0L);
```

`ensure_loaded(...)` 采用 `Tag`，例如 `"Rows"_tag`、`"(0028,FFFF)"_tag` 或 `dicom::Tag(0x0028, 0x0010)`。
它不接受关键字字符串或点分标签路径。

### 值类型和零长度行为

主要的类型化读取系列包括：

- 标量数字：`to_int()`、`to_long()`、`to_longlong()`、`to_double()`
- 向量数字/标签：`to_longlong_vector()`、`to_double_vector()`、`to_tag_vector()`
- 文本：`to_string_view()`、`to_string_views()`、`to_utf8_string()`、`to_utf8_strings()`
- 标签和 UID：`to_tag()`、`to_uid_string()`、`to_transfer_syntax_uid()`
- 人名：`to_person_name()`、`to_person_names()`

对于向量访问器，零长度值返回一个已占用的空容器而不是 `std::nullopt`：

```cpp
auto rows = ds["Rows"_tag].to_longlong_vector();         // empty vector when zero-length
auto wc = ds["WindowCenter"_tag].to_double_vector();     // empty vector when zero-length
auto at = ds["FrameIncrementPointer"_tag].to_tag_vector();
```

对于标量访问器，可以把 `std::nullopt` 视为“这个访问器无法给出值”。
如果需要区分缺失值和长度为 0 的已存在元素，请检查 `DataElement` 本身。

### 区分零长度和缺失

在 `dicomsdl` 中，`missing` 和 `zero-length` 是不同的元素状态，应在 `DataElement` 级别进行测试：

```cpp
const auto& elem = ds["PatientName"_tag];

if (!elem) {
  // 缺少查找
} else if (elem.length() == 0) {
  // 当前元素的长度值为零
} else {
  // 当前元素具有非空值
}
```

实际差异：

- 缺少元素
  - `bool(elem) == false`
  - `elem.is_missing() == true`
  - `elem.vr() == dicom::VR::None`
- 零长度当前元素
  - `bool(elem) == true`
  - `elem.is_missing() == false`
  - `elem.vr() != dicom::VR::None`
  - `elem.length() == 0`

## 数据元素

`DataElement` 是主要的元数据承载对象。

### 核心属性

- `elem.tag()`
- `elem.vr()`
- `elem.length()`
- `elem.offset()`
- `elem.vm()`
- `elem.parent()`

```cpp
const auto& elem = ds["Rows"_tag];
auto tag = elem.tag();
auto vr = elem.vr();
auto length = elem.length();
```

### 真值判定与缺失元素对象

```cpp
const auto& elem = ds["PatientName"_tag];
if (elem) {
  // 存在的元素
}

const auto& missing = ds["NotARealKeyword"];
if (!missing && missing.is_missing()) {
  // 缺少查找
}
```

对于零长度的存在元素，`bool(elem)` 仍然是 `true`；使用 `elem.length() == 0` 来检测它们。

### 类型化读写辅助函数

- `to_long()`、`to_double()`、`to_tag()`、`to_uid_string()`
- `to_string_view()`、`to_utf8_string()`、`to_utf8_strings()`
- `to_person_name()`、`to_person_names()`
- `from_long(...)`、`from_double(...)`、`from_tag(...)`
- `from_string_view(...)`、`from_utf8_view(...)`、`from_uid_string(...)`
- `from_person_name(...)`、`from_person_names(...)`

当您已经有 `DataElement&` 时，`from_xxx(...)` 帮助程序是直接写入路径。

### 容器助手

对于`SQ`和封装的像素数据：

- `elem.sequence()` / `elem.as_sequence()`
- `elem.pixel_sequence()` / `elem.as_pixel_sequence()`

将它们视为容器值，而不是标量字符串或数字。

### 原始字节和视图生命周期

`value_span()` 返回 `std::span<const std::uint8_t>` 而不复制：

```cpp
const auto& pixel_data = ds["PixelData"_tag];
auto bytes = pixel_data.value_span();
// bytes.data()、bytes.size()
```

`to_string_view()` 样式访问器也是基于视图的。
如果元素被替换或突变，视图将变得无效，因此请保持所属数据集/文件处于活动状态并在写入后刷新视图。

## 写入值

### ensure_dataelement(...)

当您想要便于链式调用的 ensure/create 行为时，请使用 `ensure_dataelement(...)`：

```cpp
auto& existing_rows = ds.ensure_dataelement("Rows"_tag);  // default vr == VR::None
ds.ensure_dataelement("Rows"_tag, dicom::VR::US).from_long(512);
ds.ensure_dataelement(
    "ReferencedStudySequence.0.ReferencedSOPInstanceUID",
    dicom::VR::UI).from_uid_string("1.2.3");
```

规则：

- 现有元素 + 省略 `vr` （默认 `VR::None`）或显式 `VR::None` -> 保留原样
- 现有元素 + 显式不同的 VR -> 重置到位
- 缺失元素 + 显式 VR -> 插入带有该 VR 的零长度元素
- 缺少标准标签 + 省略 `vr` -> 使用字典 VR 插入零长度元素
- 缺少未知/私有标签 + 省略 `vr` -> 抛出，因为没有字典 VR 需要解析

### 通过返回的 DataElement 更新现有元素

当您已经拥有该元素时，请在 `DataElement` 上使用 `from_xxx(...)`：

```cpp
if (auto& rows = ds["Rows"_tag]; rows) {
  rows.from_long(512);
}
```

如果您想要创建或更新行为，请从 `ensure_dataelement(...)` 而不是 `operator[]` 开始。

### 使用 set_value(...) 进行一次性赋值

当您希望在一次调用中完成同样的 ensure + 类型化写入流程时，请使用 `set_value(...)`：

```cpp
bool ok = true;
ok &= ds.set_value("Rows"_tag, 512L);
ok &= ds.set_value("Columns"_tag, 512L);
ok &= ds.set_value("BitsAllocated"_tag, 16L);
ok &= ds.set_value(dicom::Tag(0x0009, 0x0030), dicom::VR::US, 16L);  // private tag
```

它遵循上面 `ensure_dataelement(...)` 的同样规则，也就是现有元素 / 缺失元素的处理，以及显式 `vr` / 省略 `vr` 的行为都保持一致；之后如果编码或赋值失败，则返回 `false`。

失败模型：

- 成功时，写入请求的值
- 失败时，`set_value()` 返回 `false`
- `DataSet` / `DicomFile` 仍然可用
- 目标元素状态未指定，不应依赖

如果您需要回滚语义，请自行保留以前的值并显式恢复它。

### 创建零长度值与删除元素

零长度和删除是不同的操作。

使用 `add_dataelement(...)`、`ensure_dataelement(...)`，或者显式传入空负载，可以创建或保留长度为 0 的元素：

```cpp
ds.add_dataelement("PatientName"_tag, dicom::VR::PN);  // present element with zero-length value
ds.set_value("PatientName"_tag, std::string_view{});

std::vector<long long> empty_numbers;
ds.set_value("Rows"_tag, std::span<const long long>(empty_numbers));
```

使用`remove_dataelement(...)`进行删除：

```cpp
ds.remove_dataelement("PatientName"_tag);
ds.remove_dataelement(dicom::Tag(0x0028, 0x0010));
```

### 私有或模糊标签的显式 VR 分配

```cpp
bool ok = ds.set_value(dicom::Tag(0x0009, 0x0030), dicom::VR::US, 16L);
```

当标签是 private 的，或者您想在赋值前覆盖现有非序列元素的 VR 时，这种形式很有用。

规则：

- 如果元素丢失，则使用提供的 VR 来创建它
- 如果该元素存在并且已经具有该 VR，则该值将就地更新
- 如果该元素存在不同的非 `SQ`/非 `PX` VR，则绑定可能会替换该 VR，然后分配值
- `VR::None`、`SQ` 和 `PX` 不是此重载的有效覆盖目标

### add_dataelement(...)

当您想要显式创建/替换语义时，请使用 `add_dataelement(...)`：

```cpp
ds.add_dataelement("Rows"_tag, dicom::VR::US).from_long(512);
```

与`ensure_dataelement(...)`相比，`add_dataelement(...)`对现有元素的破坏性更大。
如果目标元素已存在，`add_dataelement(...)` 会将其重置为新的零长度元素，然后再次填充它。

当您想要显式替换行为时，请使用 `add_dataelement(...)`。
如果您想要保留现有元素，除非需要显式 VR 更改，请改用 `ensure_dataelement(...)`。
`set_value(...)` 遵循 `ensure_dataelement(...)` 路径，而不是 `add_dataelement(...)` 路径。

## 实用操作

### 迭代和大小

`for (const auto& elem : ds)` 会遍历该数据集中的元素。
`ds.size()` 返回该数据集中的元素个数。
`file->size()` 会从 `DicomFile` 转发根数据集的大小。

```cpp
for (const auto& elem : ds) {
  std::cout << elem.tag().to_string()
            << ' ' << elem.vr().str()
            << ' ' << elem.length() << '\n';
}

std::cout << "element count: " << ds.size() << '\n';
std::cout << "file count: " << file->size() << '\n';
```

代表性输出如下所示：

```text
(0002,0010) UI 20
(0010,0010) PN 8
(0028,0010) US 2
element count: 42
file count: 42
```

### dump()

`dump()` 在 `DataSet` 和 `DicomFile` 上返回制表符分隔的人类可读转储字符串。

```cpp
auto full = file->dump(80, true);
auto compact = ds.dump(40, false);
```

- `max_print_chars` 截断长 `VALUE` 预览。
- `include_offset = false` 删除 `OFFSET` 列。
- 对于基于文件的根数据集，`dump()` 在格式化输出前还会加载尚未读取的剩余元素。

代表性输出如下所示：

```text
TAG	VR	LEN	VM	OFFSET	VALUE	KEYWORD
'00020010'	UI	20	1	132	'1.2.840.10008.1.2.1'	TransferSyntaxUID
'00100010'	PN	8	1	340	'Doe^Jane'	PatientName
'00280010'	US	2	1	702	512	Rows
```

使用 `include_offset = false`，标题和列变为：

```text
TAG	VR	LEN	VM	VALUE	KEYWORD
'00100010'	PN	8	1	'Doe^Jane'	PatientName
```

## 部分加载规则

- `get_value(...)`、`get_dataelement(...)` 和 `operator[]` 不会隐式继续部分加载。
- 尚未解析的数据元素将表现为缺失，直到您调用 `ensure_loaded(tag)`。
- 当目标数据元素尚未解析时，`add_dataelement(...)`、`ensure_dataelement(...)` 和 `set_value(...)` 会抛出异常。
- staged read 之后如果还需要更后面的标签，请先显式推进加载边界，再继续读取或写入。

## 附加说明

### 性能说明

- 对于热路径中常见的单个标签访问，与运行时文本解析相比，更推荐 `"_tag"` 字面量或重用 `dicom::Tag` 对象。
- 当您只需要类型化结果而不需要 `DataElement` 元数据时，优先使用 `get_value<T>(tag, default)`。
- 如果字符串/路径形式能让嵌套访问更清晰，就使用它。对于热点循环中的重复查找，最好缓存标签，或者把遍历过程显式拆开。

### 可运行的示例

- `examples/dataset_access_example.cpp`
- `examples/batch_assign_with_error_check.cpp`
- `examples/dump_dataset_example.cpp`
- `examples/tag_lookup_example.cpp`
- `examples/keyword_lookup_example.cpp`

## 相关文档

- [核心对象](core_objects.md)
- [文件I/O](file_io.md)
- [序列和路径](sequence_and_paths.md)
- [Python数据集指南](python_dataset_guide.md)
- [C++ API概述](../reference/cpp_api.md)
- [数据集参考](../reference/dataset_reference.md)
- [数据元素参考](../reference/dataelement_reference.md)
- [错误处理](error_handling.md)
