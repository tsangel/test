# Python 数据集指南

DicomSDL 是一个轻量的 nanobind 包装层。它会在运行时加载原生扩展，因此文档通过 `mock import` 构建；运行示例时请先安装 wheel。

这是 DicomSDL 中 Python 端文件、数据集和元素访问的主要面向用户指南。
它涵盖了模块级入口点、DicomSDL 对象如何映射到 DICOM 以及最重要的读/写模式。

## 导入

```python
import dicomsdl as dicom
```

## 模块级入口点

- `keyword_to_tag_vr(keyword: str) -> (Tag, VR)`：将关键字解析为`(Tag, VR)`。
- `tag_to_keyword(tag: Tag | str) -> str`：将标签解析为关键字。
- `read_file(path) -> DicomFile`：从磁盘加载 DICOM 文件/会话。
- `read_bytes(data, name="inline") -> DicomFile`：从内存缓冲区加载。
- `read_json(source, name="<memory>", charset_errors="strict") -> list[(DicomFile, list[JsonBulkRef])]`：读取内存中已有的 UTF-8 DICOM JSON 文本或字节。
- `generate_uid() -> str`：在 DICOMSDL 前缀下创建一个新的 UID。
- `append_uid(base_uid: str, component: int) -> str`：附加一个具有后备策略的 UID 组件。

关于 `write_json(...)`、`BulkDataURI`、presigned URL 或带签名/令牌的下载
URL 保留以及 `set_bulk_data(...)` 等 JSON 专用流程，请参阅
[DICOM JSON](dicom_json.md)。

## DicomSDL 如何映射到 DICOM

DicomSDL 公开了一小组相关的 Python 对象：

- `DicomFile`：拥有根数据集的文件/会话包装器
- `DataSet`：DICOM `DataElement` 对象的容器
- `DataElement`：一个 DICOM 字段，带有标签/VR/长度元数据和类型值访问
- `Sequence`：`SQ` 值的嵌套项目容器
- `PixelSequence`：用​​于封装或压缩像素数据的帧/片段容器

有关对象模型和 DICOM 映射，请参阅[核心对象](core_objects.md)。

该绑定有意使用拆分模型：

- 属性访问是普通顶层 keyword 取值的主路径：`ds.Rows`、`df.PatientName`
- 索引访问返回 `DataElement`：`ds["Rows"]`
- `get_value(...)` 是面向动态键、点分 tag path 和自定义默认值的显式取值路径

这样既能让常见读取保持简洁，也仍然便于检查嵌套遍历以及 VR / 长度 / 标签元数据。

### DicomFile 和数据集

大多数数据元素访问 API 都是在 `DataSet` 上实现的。
`DicomFile` 拥有根 `DataSet` 并处理面向文件的工作，例如加载、保存和转码。
为了方便起见，`DicomFile` 转发根数据集访问，因此 `df.Rows`、`df.PatientName`、`df["Rows"]`、`df.get_value(...)` 和 `df.Rows = 512` 均委托给 `df.dataset`。
如果绑定 `ds = df.dataset`，则直接使用相同的数据集 API，无需转发。

这些模式是等效的：

```python
df = dicom.read_file("sample.dcm")

rows1 = df.Rows
rows2 = df.dataset.Rows

elem1 = df["Rows"]
elem2 = df.dataset["Rows"]

df.Rows = 512
df.dataset.Rows = 512
```

## 推荐 API

|应用程序接口 |返回|缺失行为 |预期用途 |
| --- | --- | --- | --- |
| `"Rows" in ds` | `bool` | `False` |存在探针|
| `ds.Rows` | 类型化值或 `None` | 有效的 DICOM keyword 缺失时返回 `None`；未知 keyword 抛出 `AttributeError` | 在 `DataSet` / `DicomFile` 上读取标准 DICOM keyword 的推荐顶层路径 |
| `ds.get_value("Rows", default=None)` |类型化值或传入的默认值 |返回传入的默认值 |用于动态键、点分 tag path 或自定义默认值的显式一次性类型化读取 |
| `ds["Rows"]`、`ds.get_dataelement("Rows")` | `DataElement` |返回会被判定为 `False` 的 `NullElement`，不抛异常 | `DataElement` 访问 |
| `ds.ensure_loaded("Rows")` | `None` |因无效键而引发 |显式继续部分读取直到后面的标签，例如 `Rows` |
| `ds.ensure_dataelement("Rows", vr=None)` | `DataElement` |返回现有或插入零长度元素 |链接友好的确保/创建 API |
| `ds.set_value("Rows", 512)` | `bool` |写入或零长度|一次性类型化写入 |
| `ds.set_value(0x00090030, dicom.VR.US, 16)` | `bool` |通过显式 VR 创建或覆盖 |私人或不明确的标签 |
| `ds.Rows = 512` | `None` |赋值失败时引发异常 |面向开发/交互环境、用于标准关键字更新的便捷赋值 |

## 在 Python 中识别数据元素的方法

| 形式 | 示例 | 优先考虑的场景 |
| --- | --- | --- |
| 打包整数 | `0x00280010` | 标签已经来自数字表或外部元数据时 |
| 关键字或标签字符串 | `"Rows"`, `"(0028,0010)"` | 想使用显式字符串键，或该键不适合写成 Python 属性时 |
| 点分标签路径字符串 | `"RadiopharmaceuticalInformationSequence.0.RadionuclideTotalDose"` | 想一步完成嵌套查找或赋值时 |
| `Tag` 对象 | `dicom.Tag("Rows")`, `dicom.Tag(0x0028, 0x0010)` | 想要显式且可复用的标签对象时 |

### `0x00280010`

- 当标签已经来自数字常量、生成表或外部元数据时，用它。
- 优点: 对普通标签来说最快也最直接，没有字符串解析，并且 `ensure_loaded(...)` 这类普通标签 API 也能用。
- 权衡: 可读性不如关键字，不能表达嵌套路径。

### `"Rows"` 或 `"(0028,0010)"`

- 当你想用显式字符串键而不是属性访问时，优先用它。
- 优点: 简短、可读，适用于常见的查找 / 写入 API。
- 权衡: 运行时会有一点关键字/标签字符串解析成本；嵌套访问需要点分路径字符串。

### 点分标签路径字符串

- 当你想一步完成嵌套查找或赋值时，用它。
- 示例: `"RadiopharmaceuticalInformationSequence.0.RadionuclideTotalDose"`, `"00540016.0.00181074"`。
- 优点: 对嵌套 dataset 提供可读的一次性访问。
- 权衡: 只有支持嵌套路径的 API 才接受它；`ensure_loaded(...)`、`remove_dataelement(...)` 这类普通标签 API 不支持。

### `dicom.Tag(...)`

- 当你想要一个显式标签对象，或要在多个调用之间复用同一个标签时，用它。
- 优点: 类型明确、可复用、适合放在 API 边界上使用。
- 权衡: 一次性调用时会多一次 Python 层的 `Tag` 对象构造；如果不需要复用，packed int 更直接。

实用推荐：

- 对普通顶层标准 DICOM keyword 读取，优先使用 `ds.Rows` / `df.PatientName`。这样常见代码更短，在当前绑定上性能也不错；如果已知 keyword 只是缺失，会返回 `None`，而未知 keyword 或普通拼写错误仍然会抛出 `AttributeError`。
- 当键是动态的、需要自定义默认值，或者要读取 `"Seq.0.Tag"` 这样的点分 tag path 时，请使用 `get_value(...)`。
- 当单个标签已经来自数值常量或外部元数据，或者你想要单标签的最快路径时，请使用 packed int。
- 当你想一步完成嵌套值读取或赋值时，请使用点分 tag path 字符串。在 Python 中，这也可能比反复调用嵌套 `Sequence` / `DataSet` API 更快，因为遍历保持在一次 C++ 路径解析 / 查找调用中。
- 当你需要显式且可复用的标签对象时，请使用 `dicom.Tag(...)`。
- 当你需要 VR、长度、标签、sequence、raw byte 等 `DataElement` 本身的元数据时，请使用 `ds["Rows"]` 或 `get_dataelement(...)`。

## 读取值

### 属性访问返回类型化值

```python
rows = df.Rows
patient_name = ds.PatientName
window_center = ds.WindowCenter  # 如果 keyword 有效但当前缺失，则为 None
```

当你在 `DataSet` 或 `DicomFile` 上读取普通顶层标准 DICOM keyword，并且需要实际值而不是 `DataElement` 元数据时，优先使用这一条路径。

如果有效的 DICOM keyword 当前缺失，结果就是 `None`。
未知属性名仍然会抛出 `AttributeError`，因此普通拼写错误不会被悄悄吞掉。

### 索引访问返回一个 DataElement

```python
elem = ds["Rows"]
if elem:
    print(elem.tag, elem.vr, elem.length, elem.value)
```

缺失查找会返回一个会被判定为 `False` 的对象，而不是抛出异常：

```python
missing = ds["NotARealKeyword"]
assert not missing
assert missing.value is None
```

### 存在检查

当你只需要知道一个元素是否存在时，使用`in`：

```python
if "Rows" in ds:
    rows = ds["Rows"].value

if dicom.Tag("PatientName") in df:
    print(df["PatientName"].value)
```

接受的键类型有：

- `str` 关键字或标签路径字符串
- `Tag`
- 打包 `int` 例如 `0x00280010`

格式错误的关键字/标签字符串返回 `False`。

### 方法形式的相同查找：get_dataelement(...)

`get_dataelement(...)` 执行与 `ds[...]` 相同的查找。有些代码库更喜欢命名方法形式，因为它读起来更清晰：

```python
elem = ds.get_dataelement("PatientName")
if elem:
    print(elem.vr, elem.length, elem.value)
```

它在缺失元素时与 `ds[...]` 的行为相同。

### 部分负载继续

当部分读取在您现在需要的标签之前停止时，请使用 `ensure_loaded(...)`：

```python
df.ensure_loaded("Rows")
df.dataset.ensure_loaded(dicom.Tag("Columns"))
```

接受的键类型有：

- `Tag`
- 打包 `int` 例如 `0x00280010`
- 关键字或标签字符串，例如 `"Rows"` 或 `"(0028,0010)"`

`ensure_loaded(...)` 不支持嵌套的点分标签路径字符串。

### 显式取值路径：get_value()

当键是动态的、需要显式默认值，或者要读取点分 tag path 时，请使用 `get_value()`：

```python
keyword = "Rows"
rows = ds.get_value(keyword)
window_center = ds.get_value("WindowCenter", default=None)
total_dose = ds.get_value(
    "RadiopharmaceuticalInformationSequence.0.RadionuclideTotalDose",
    default=None,
)
```

当你不需要 `DataElement` 对象时，这是最显式的不抛异常取值路径。
如果元素缺失，你会得到传入的 `default`。

`get_value()` 不会隐式继续部分加载。如果文件支持的数据集是
仅加载到较早的标签，查询较晚的标签将返回当前可用的标签
状态。当部分读取后需要稍后的标签（例如 `Rows`）时，请先调用 `ensure_loaded(...)`。

传入的默认值仅用于缺失元素。具有零长度值的数据元素仍然返回类型化的空值：

```python
assert ds.get_value("PatientName", default="DEFAULT") == "DEFAULT"  # missing
assert ds.get_value("Rows", default="DEFAULT") == []                # present, zero-length US
```

### 链接路径：ds["Rows"].value

当您需要同时使用元数据和值时，请使用此选项：

```python
rows_elem = ds["Rows"]
if rows_elem:
    print(rows_elem.vr)
    print(rows_elem.value)
```

### `DataElement.value` / `get_value()` 的返回类型

- `SQ` / `PX` -> `Sequence` / `PixelSequence`
- 类似数字的 VR (`IS`、`DS`、`AT`、`FL`、`FD`、`SS`、`US`、`SL`、`UL`、`SV`、`UV`) -> `int`、`float`、`Tag` 或 `list[...]`
- `PN` -> 解析成功时为`PersonName`或`list[PersonName]`
- 字符集感知文本 VR -> UTF-8 `str` 或 `list[str]`
- 字符集解码或 `PN` 解析失败 -> 原始 `bytes`
- 二进制 VR -> 只读 `memoryview`

对于长度值为零的数据元素：

- 类似数字的 VR 返回 `[]`
- 文本 VR 返回 `""`
- `PN` 返回空 `PersonName`
- 二进制 VR 返回空的只读 `memoryview`
- `SQ` / `PX` 返回空容器对象

这与底层 C++ 向量访问器相匹配：零长度类似数字的值被视为可解析的空向量，而不是缺失值。

### 零长度返回矩阵

最重要的规则是长度值为零的数据元素仍然“存在”。它不使用 `default` 参数，并且它的行为不像丢失查找。

特别是，某些字符串 VR 通常可以具有 `VM > 1`，但零长度值仍会读回为空标量样式值，因为 `vm()` 是 `0`，而不是 `> 1`。

| VR家庭|非空 `VM == 1` |非空 `VM > 1` |零长度值 |
| --- | --- | --- | --- |
| `AE`、`AS`、`CS`、`DA`、`DT`、`TM`、`UI`、`UR` | `str` | `list[str]` | `""` |
| `LO`、`LT`、`SH`、`ST`、`UC`、`UT` | `str` | `list[str]` 用于支持多值的 VR；否则 `str` | `""` |
| `PN` | `PersonName` |解析成功时为`list[PersonName]` |空 `PersonName` |
| `IS`、`DS` | `int` / `float` | `list[int]` / `list[float]` | `[]` |
| `AT` | `Tag` | `list[Tag]` | `[]` |
| `FL`、`FD`、`SS`、`US`、`SL`、`UL`、`SV`、`UV` | `int` / `float` | `list[int]` / `list[float]` | `[]` |
| `OB`、`OD`、`OF`、`OL`、`OW`、`OV`、`UN` | `memoryview` |不用作 Python 列表值 |空 `memoryview` |
| `SQ`、`PX` |序列对象 |类似序列的容器|空容器对象|

示例：

```python
assert ds.get_value("ImageType") == ["ORIGINAL", "PRIMARY"]
assert ds.get_value("ImageType", default="DEFAULT") == ""   # present, zero-length CS

assert ds.get_value("PatientName", default="DEFAULT") == "DEFAULT"  # missing
assert str(ds["PatientName"].value) == ""                           # present, zero-length PN

assert ds.get_value("Rows", default="DEFAULT") == []               # present, zero-length US
assert ds.get_value("WindowCenter", default="DEFAULT") == []       # present, zero-length DS
```

对于直接向量访问器，零长度值也返回空容器而不是 `None`：

```python
assert ds["Rows"].to_longlong_vector() == []
assert ds["WindowCenter"].to_double_vector() == []
assert ds["FrameIncrementPointer"].to_tag_vector() == []
```

在 C++ 层，相同的约定现在适用于向量访问器：

```cpp
auto rows = dataset["Rows"_tag].to_longlong_vector();   // engaged optional, empty vector when zero-length
auto wc = dataset["WindowCenter"_tag].to_double_vector();
auto at = dataset["FrameIncrementPointer"_tag].to_tag_vector();
```

### 区分零长度和缺失

在 DicomSDL 中，`missing` 和 `zero-length` 是不同的元素状态，应在 `DataElement` 级别进行测试，而不是仅查看 `elem.value`。

使用这个规则：

```python
elem = ds["PatientName"]

if not elem:
    # 缺少查找
elif elem.length == 0:
    # 当前元素的长度值为零
else:
    # 当前元素具有非空值
```

实际差异：

- 缺少元素
  - `bool(elem) == False`
  - `elem.is_missing() == True`
  - `elem.vr == dicom.VR.None`
  - `elem.value is None`
- 零长度当前元素
  - `bool(elem) == True`
  - `elem.is_missing() == False`
  - `elem.vr != dicom.VR.None`
  - `elem.length == 0`

这种区别对于 DICOM 属性很重要，其中“存在但为空”在语义上与“不存在”不同。

## 数据元素

`DataElement` 是主要的元数据承载对象。

### 核心属性

- `elem.value`
- `elem.tag`
- `elem.vr`
- `elem.length`
- `elem.offset`
- `elem.vm`

这些是属性，而不是方法调用：

```python
elem = ds["Rows"]
print(elem.tag)
print(elem.vr)
print(elem.length)
```

### 真值判定与缺失元素对象

```python
elem = ds["PatientName"]
if elem:
    ...

missing = ds["NotARealKeyword"]
assert not missing
assert missing.is_missing()
```

`bool(elem)` 与 `elem.is_present()` 匹配。

对于零长度的存在元素，`bool(elem)` 仍然是 `True`；使用 `elem.length == 0` 来检测它们。

### 类型化读写辅助函数

- `elem.get_value()` 镜像 `elem.value`
- `elem.set_value(value)` 镜像 `value` setter 并返回 `True`/`False`
- 失败的 `elem.set_value(value)` 使所属数据集有效，但目标元素状态未指定
- 类型转换助手包括 `to_long()`、`to_double()`、`to_string_view()`、`to_utf8_string()`、`to_utf8_strings()`、`to_person_name()` 以及对应的向量变体

### 原始字节

`value_span()` 返回只读 `memoryview`，无需复制：

```python
raw = ds.get_dataelement("PixelData").value_span()
print(raw.nbytes)
```

如果需要拥有独立生命周期的 Python `bytes`，请使用 `value_bytes()`。
如果你已经拿到了 `memoryview`，也可以用 `bytes(view)` 得到同类复制结果，
但直接调用 `value_bytes()` 更直接。

```python
elem = ds.get_dataelement("PixelData")

view = elem.value_span()      # zero-copy 只读 memoryview
blob = elem.value_bytes()     # 复制得到的 bytes
```

以下情况优先考虑 `value_span()`：

- 下游消费者可以直接接受 buffer protocol（`memoryview`、NumPy、`struct`、切片）
- payload 可能较大
- 你希望避免立即复制

以下情况优先考虑复制后的 bytes（`value_bytes()`）：

- 下一个 API 明确要求 `bytes`
- 你需要一个拥有独立生命周期的不可变副本
- payload 很小，复制成本几乎可以忽略

Python 绑定的经验性性能指导：

- 对非常小的 payload，`memoryview` / exporter 的创建成本可能让 copied bytes 更快
- 大致在 `2 KiB` 以下时，copied bytes 往往更快或至少很有竞争力
- 大致从 `4 KiB` 起，通常 `value_span()` 更有优势
- `64 KiB+` 时，`value_span()` 的优势会很明显
- 这些阈值来自当前 Python binding benchmark 的经验结果，不是固定 ABI 保证

## 写入值

### 确保或创建查找

`ensure_dataelement(...)` 是链接友好的“确保此元素存在”API：

```python
rows = ds.ensure_dataelement("Rows")
private_value = ds.ensure_dataelement(0x00090030, dicom.VR.US)
```

规则：

- 如果元素已经存在并且省略 `vr` 或 `None`，则现有元素原封不动地返回
- 如果元素已经存在并且 `vr` 是显式的但不同，则重置现有元素
到位，以便保证所请求的 VR
- 如果元素丢失，则插入一个新的零长度元素
- 与 `add_dataelement(...)` 不同，此 API 仅在必须强制执行显式 V​​R 时重置
- 在部分加载的文件支持数据集上，调用 `ensure_dataelement(...)` 来获取其标签
数据元素尚未解析，而不是隐式继续加载
### 通过返回的 DataElement 更新现有元素

```python
ds["Rows"].value = 512
```

一旦你已经有了元素对象，这就是自然的路径。

### 使用 set_value() 进行一次性赋值

```python
assert ds.set_value("Rows", 512)
assert ds.set_value("StudyDescription", "Example")
assert ds.set_value("Rows", None)   # present, zero-length US
```

当您希望在一次调用中通过按键创建/更新时，这是最佳路径。

在文件支持的部分加载数据集上，`set_value(...)` 不会因为目标标签而自动继续加载。
如果目标数据元素尚未解析，它的行为与 `add_dataelement(...)` / `ensure_dataelement(...)` 一致。

失败模型：

- 成功时，写入请求的值
- 失败时，`set_value()` 返回 `False`
- `DataSet` / `DicomFile` 仍然可用
- 目标元素状态未指定，不应依赖

如果您需要回滚语义，请自行保留以前的值并显式恢复它。

### 创建零长度值与删除元素

`None` 表示该元素存在，但值长度为 0：

```python
assert ds.set_value("PatientName", None)   # present, zero-length PN
assert ds.set_value("Rows", None)          # present, zero-length US
```

`None` 只是符合已解析 VR 的零长度表示的简写。
您也可以用显式的空载荷表达同样的意图：

```python
ds.add_dataelement(dicom.Tag("PatientName"), dicom.VR.PN)   # present, zero-length

assert ds.set_value("PatientName", "")      # zero-length text element
assert ds.set_value("Rows", [])             # zero-length numeric VM-based element
assert ds.set_value(0x00111001, dicom.VR.OB, b"")  # zero-length binary element
```

相同的规则适用于现有元素：

```python
ds["PatientName"].value = None
ds["PatientName"].value = ""
ds["Rows"].value = None
ds["Rows"].value = []
```

建议这样理解：

- `None` -> 保留现有元素，或创建一个长度为 0 的元素
- 空载荷（`""`、`[]`、`b""`）-> 让元素继续存在，并保持 `length == 0`

使用`remove_dataelement()`进行删除：

```python
ds.remove_dataelement("PatientName")
ds.remove_dataelement(0x00280010)
ds.remove_dataelement(dicom.Tag("Rows"))
```

### 为私有标签或有歧义的标签显式指定 VR

```python
assert ds.set_value(0x00090030, dicom.VR.US, 16)
```

当标签是私有的，或者您想在赋值前覆盖现有的非序列元素 VR 时，这种形式很有用。

规则：

- 如果元素丢失，则使用提供的 VR 来创建它
- 如果该元素存在并且已经具有该 VR，则该值将就地更新
- 如果该元素存在不同的非 `SQ`/非 `PX` VR，则绑定可能会替换该 VR，然后分配值
- `VR.None`、`SQ` 和 `PX` 不是这种形式的有效覆盖目标

这种形式不提供回滚语义。如果返回 `False`，则数据集
仍然有效，但目标元素状态未指定。

### 属性赋值的便捷写法

```python
ds.Rows = 512
df.PatientName = pn
```

对于标准的关键字更新，属性赋值仍然是一种面向值的便捷写法。
它写起来更简洁，适用于 `DataSet`，也适用于通过 `DicomFile` 转发出来的同类访问。
与 `set_value(...)` 不同，这条路径在赋值失败时会抛出异常，而不是返回 `False`，
因此它通常更适合开发、笔记本和交互式使用，而不是
需要显式错误处理的生产代码。

## 实用操作

### 迭代和大小

`for elem in ds` 迭代该数据集中的当前元素。
`ds.size()` 返回该数据集的元素计数。
`len(df)` 返回 `DicomFile` 根数据集的大小。

```pycon
>>> for elem in ds:
...     print(elem.tag, elem.vr, elem.length)
(0002,0010) UI 20
(0010,0010) PN 8
(0028,0010) US 2
>>> ds.size()
42
>>> len(df)
42
```

当您已经拥有数据集对象时，请使用 `ds.size()`。
当您仍在使用文件对象时，请使用 `len(df)`。

### dump()

`dump()` 会在 `DicomFile` 和 `DataSet` 上返回一段制表符分隔、便于人阅读的转储字符串。

```python
full_text = df.dump(max_print_chars=80, include_offset=True)
compact_text = ds.dump(max_print_chars=40, include_offset=False)
```

- `max_print_chars` 截断长 `VALUE` 预览。
- `include_offset=False` 删除 `OFFSET` 列。
- 对于基于文件的根数据集，`dump()` 在格式化转储前还会加载尚未读取的剩余元素。

代表性输出如下所示：

```text
TAG	VR	LEN	VM	OFFSET	VALUE	KEYWORD
'00020010'	UI	20	1	132	'1.2.840.10008.1.2.1'	TransferSyntaxUID
'00100010'	PN	8	1	340	'Doe^Jane'	PatientName
'00280010'	US	2	1	702	512	Rows
```

使用 `include_offset=False` 时，表头和列会变成：

```text
TAG	VR	LEN	VM	VALUE	KEYWORD
'00100010'	PN	8	1	'Doe^Jane'	PatientName
```

## 附加说明

### 性能说明

- 关键字和标签查找使用恒定时间字典路径。
- 在大文件中，优先选择目标元素访问而不是 Python 热循环中的完整迭代。

### 像素变换元数据

帧感知元数据解析：

- `DicomFile.rescale_transform_for_frame(frame_index)`
- `DicomFile.window_transform_for_frame(frame_index)`
- `DicomFile.voi_lut_for_frame(frame_index)`
- `DicomFile.modality_lut_for_frame(frame_index)`

记录在[像素变换元数据解析](../reference/pixel_transform_metadata.md)中。

### 可运行的示例

- `examples/python/dataset_access_example.py`
- `examples/python/dump_dataset_example.py`

## 相关文档

- C++ 对应部分：[C++ 数据集指南](cpp_dataset_guide.md)
- 输入输出行为：[文件I/O](file_io.md)
- 文件级 API 接口：[DicomFile 参考](../reference/dicomfile_reference.md)
- `DataElement` 详细信息：[DataElement 参考](../reference/dataelement_reference.md)
- `Sequence` 遍历：[序列参考](../reference/sequence_reference.md)
- 异常和失败类别：[错误处理](error_handling.md)
- 解码像素输出：[Pixel Decode](pixel_decode.md)
- 文本 VR 和 `PN`：[字符集和人名](charset_and_person_name.md)
- 支持Python类型：[Python API参考](../reference/python_reference.md)
- UID生成详细信息：[生成UID](generating_uid.md)
- 像素编码限制：[像素编码约束](../reference/pixel_encode_constraints.md)
