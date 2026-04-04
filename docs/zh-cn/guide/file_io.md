# 文件输入/输出

本页涵盖磁盘和内存输入、部分加载以及文件、字节和流的主要输出路径。

关于 DICOM JSON Model 的读写，包括 `read_json(...)`、`write_json(...)`、
`BulkDataURI` 和 `set_bulk_data(...)`，请参阅
[DICOM JSON](dicom_json.md)。

## 文件 I/O 的工作原理

- `read_file(...)` 和 `read_bytes(...)` 创建 `DicomFile` 并立即解析输入到 `load_until`。
- `write_file(...)` 和 `write_bytes(...)` 将 `DicomFile` 对象序列化为文件或字节。
- `write_with_transfer_syntax(...)` 是面向输出的转码路径，用于使用不同的传输语法直接写入文件或流。这通常是您在更改像素压缩（例如更改为 `HTJ2KLossless`）时所需要的。它不会首先改变源对象。在 C++ 中，同一 API 系列也有流重载。

## 从磁盘读取

**C++**

```cpp
#include <dicom.h>

auto file = dicom::read_file("in.dcm");
```

**Python**

```python
import dicomsdl as dicom

df = dicom.read_file("in.dcm")
```

笔记：
- 当您需要的标签靠近文件的前面并且您不需要立即使用未读的尾部时，请使用 `load_until`。它可以避免预先解析完整数据集并降低读取成本。
- 稍后标签访问不会隐式继续解析。在 C++ 和 Python 中，当您稍后需要更多内容时，请调用 `ensure_loaded(tag)`。在 C++ 中，`ensure_loaded(...)` 接受 `Tag`，例如 `"Rows"_tag`、`"(0028,FFFF)"_tag` 或 `dicom::Tag(0x0028, 0x0010)`。在 Python 中，`ensure_loaded(...)` 接受 `Tag`、打包的 `int` 或单个标签的关键字字符串；不支持点分标签路径字符串。
- 当您希望保留部分读取的数据而不是立即异常时，请使用 `keep_on_error=True`；然后检查 `has_error` 和 `error_message`。
- 在Python中，`path`接受`str`和`os.PathLike`。在 C++ 中，磁盘路径 API（例如 `read_file(...)`、`write_file(...)` 和 `write_with_transfer_syntax(...)`）采用 `std::filesystem::path`。

## 从内存中读取

**C++**

```cpp
#include <dicom.h>
#include <vector>

std::vector<std::uint8_t> payload = /* full DICOM byte stream */;
auto file = dicom::read_bytes("in-memory in.dcm", std::move(payload));
```

**Python**

```python
from pathlib import Path
import dicomsdl as dicom

payload = Path("in.dcm").read_bytes()
df = dicom.read_bytes(payload, name="in-memory in.dcm")
```

笔记：
- `name` 成为 `path()` / `path` 和诊断报告的标识符。
- `load_until` 的行为方式与内存输入相同：当您只需要数据集的早期部分但未读的尾部数据稍后不会隐式加载时很有用。
- 在 Python 中，`read_bytes(..., copy=False)` 会保留对调用者缓冲区的引用而不是复制。只要 `DicomFile` 仍在引用该缓冲区，就要让它保持有效，并且不要修改其内容。
- 在 C++ 中，`read_bytes(...)` 可以从原始指针复制或获取移动的 `std::vector<std::uint8_t>` 的所有权。

## 分阶段阅读

**C++**

```cpp
#include <dicom.h>
using namespace dicom::literals;

dicom::ReadOptions opts;
opts.load_until = "0028,ffff"_tag;

auto file = dicom::read_file("in.dcm", opts);  // initial partial parse

auto& ds = file->dataset();
ds.ensure_loaded("PixelData"_tag);  // later, advance farther
```

**Python**

```python
import dicomsdl as dicom

df = dicom.read_file("in.dcm", load_until=dicom.Tag("0028,ffff"))
df.ensure_loaded("PixelData")
```

笔记：
- 当您希望初始解析提前停止时设置 `options.load_until`。
- 部分读取后，使用 `ensure_loaded(tag)` 解析更多数据元素。在 C++ 中，传递 `Tag`，例如 `"Rows"_tag`、`"(0028,FFFF)"_tag` 或 `dicom::Tag(...)`。
- 在部分加载的数据集上，尚未解析的数据元素不会隐式加载以供以后查找或写入。
- 在 Python 中，`ensure_loaded(...)` 接受 `Tag`、打包的 `int` 或单个标签的关键字字符串。不支持嵌套的点分标签路径字符串。
- 相同的分阶段读取模式适用于 `read_bytes(...)`；当您需要零复制内存输入时，请使用 `copy=false`。
- 对于 `read_bytes(..., copy=false)`，调用者拥有的缓冲区必须比 `DicomFile` 的寿命长。

## 部分加载和许可读取

- `load_until` 在读取请求的标签（包括）后停止解析。
- `keep_on_error` 保留部分读取的数据，并将读取失败记录在`DicomFile` 上。
- 在从文件或内存加载的部分加载的数据集上，查找和突变 API 不会隐式继续加载尚未解析的数据元素。
- 实际上，这意味着稍后的标签访问可能表现为丢失或引发，并且稍后的标签写入可能会引发而不是默默地改变未读数据。

**C++**

```cpp
#include <dicom.h>
using namespace dicom::literals;

dicom::ReadOptions opts;
opts.load_until = "0002,ffff"_tag;  // stop after file meta

auto file = dicom::read_file("in.dcm", opts);
auto& ds = file->dataset();

ds.ensure_loaded("0028,0011"_tag);  // advance through Columns

long rows = ds.get_value<long>("0028,0010"_tag, -1L);  // Rows
long cols = ds.get_value<long>("0028,0011"_tag, -1L);  // Columns
long bits = ds.get_value<long>("0028,0100"_tag, -1L);  // BitsAllocated

// 行和列现在可用
// 位仍然是 -1，因为 (0028,0100) 尚未被解析

ds.ensure_loaded("0028,ffff"_tag);
bits = ds.get_value<long>("0028,0100"_tag, -1L);  // now available
```

笔记：
- 当您需要的标签聚集在数据集前面附近时，这非常有用。
- 它也非常适合快速扫描许多 DICOM 文件，例如构建元数据索引或数据库，而无需触及完整数据集或像素有效负载。
- Python 支持普通标签和关键字的相同 `ensure_loaded(...)` 延续模式。

## 将 `DicomFile` 对象序列化为文件或字节

**C++**

```cpp
#include <dicom.h>

auto file = dicom::read_file("in.dcm");

dicom::WriteOptions opts;
opts.include_preamble = true;
opts.write_file_meta = true;
opts.keep_existing_meta = false;

file->write_file("out.dcm", opts);
auto payload = file->write_bytes(opts);
```

**Python**

```python
import dicomsdl as dicom

df = dicom.read_file("in.dcm")
payload = df.write_bytes(keep_existing_meta=False)
df.write_file("out.dcm", keep_existing_meta=False)
```

笔记：
- 使用默认选项，`write_file()` 和 `write_bytes()` 生成带有前导码和文件元信息的正常第 10 部分样式输出。
- `write_file_meta=False` 省略文件元组。
- `include_preamble=False` 省略 128 字节前导码。
- `keep_existing_meta=False` 在写入之前重建文件元。当您希望在序列化之前显式发生该步骤时，请使用 `rebuild_file_meta()`。
- 这些 API 将 `DicomFile` 对象序列化为文件或字节。它们不提供单独的仅输出转码路径。

## 直接转码为文件或流

**C++**

```cpp
#include <dicom.h>

auto file = dicom::read_file("in.dcm");
file->write_with_transfer_syntax(
    "out_htj2k_lossless.dcm",
    dicom::uid::WellKnown::HTJ2KLossless
);
```

**Python**

```python
from pathlib import Path
import dicomsdl as dicom

df = dicom.read_file("in.dcm")
df.write_with_transfer_syntax(Path("out_htj2k_lossless.dcm"), "HTJ2KLossless")
```

笔记：
- `write_with_transfer_syntax(...)` 使用目标传输语法直接转码到输出。这通常用于更改像素压缩，例如更改为 `HTJ2KLossless`，而不改变源 `DicomFile`。
- 当真正的目标是输出文件或流时更喜欢它，特别是对于大像素有效负载。它可以通过避免内存中的转码路径来减少峰值内存使用，该路径使解码工作缓冲区和重新编码的目标 `PixelData` 的存活时间超过所需的时间。
- 在Python中，`write_with_transfer_syntax(...)`是基于路径的仅输出转码API。在 C++ 中，同一 API 系列还支持直接流输出。
- 可查找输出可以在需要时回补 `ExtendedOffsetTable` 数据。不可搜索的输出仍然有效 DICOM，但可以省略这些表并使用空的基本偏移表。
- 典型的可查找输出是本地磁盘上的常规文件。典型的不可查找输出是管道、套接字、stdout、HTTP 响应流或 zip-entry 样式流。

## 我应该使用哪个 API？

- 本地文件，立即解析到请求的边界：`read_file(...)`
- 内存中已存在的字节：`read_bytes(...)`
- Python 中的零拷贝内存输入：`read_bytes(..., copy=False)`
- C++ 中文件支持的分阶段读取：`read_file(...)` 和 `load_until`，然后是 `ensure_loaded(...)`
- C++ 中从内存进行零复制分阶段读取：`read_bytes(...)` 与 `copy=false` 和可选的 `load_until`，然后是 `ensure_loaded(...)`
- 将 `DicomFile` 对象序列化为文件或字节：`write_file(...)` 或 `write_bytes(...)`
- 将新的传输语法直接写入路径：`write_with_transfer_syntax(...)`
- 在 C++ 中，将新的传输语法直接写入输出流：`write_with_transfer_syntax(...)`

## 相关文档

- [DICOM JSON](dicom_json.md)
- [核心对象](core_objects.md)
- [C++ 数据集指南](cpp_dataset_guide.md)
- [Python数据集指南](python_dataset_guide.md)
- [像素解码](pixel_decode.md)
- [像素编码](pixel_encode.md)
- [C++ API概述](../reference/cpp_api.md)
- [数据集参考](../reference/dataset_reference.md)
- [Dicom文件参考](../reference/dicomfile_reference.md)
- [错误处理](error_handling.md)
