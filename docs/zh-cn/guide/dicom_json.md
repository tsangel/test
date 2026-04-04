# DICOM JSON

DicomSDL 可以把 `DataSet` 或 `DicomFile` 序列化为 DICOM JSON Model，
也可以把 DICOM JSON 再读回为一个或多个 `DicomFile` 对象。

本页聚焦于 Python 和 C++ 共用的公开工作流：
`write_json(...)`、`read_json(...)` 和 `set_bulk_data(...)`。

## 支持范围

- `DicomFile.write_json(...)`
- `DataSet.write_json(...)`
- 从内存中已有的 UTF-8 文本或字节调用 `read_json(...)`
- DICOM JSON top-level object 和 top-level array payload
- `BulkDataURI`、`InlineBinary`、嵌套 sequence 和 PN object
- 由调用方管理 bulk 下载的 `JsonBulkRef` + `set_bulk_data(...)`

当前范围说明：

- `read_json(...)` 是内存输入 API，不直接从磁盘或 HTTP 流读取。
- JSON reader / writer 实现的是 DICOM JSON Model，而不是完整的
  DICOMweb HTTP 客户端或服务器栈。

## 写出 JSON

### Python write example

```python
import dicomsdl as dicom

df = dicom.read_file("sample.dcm")

json_text, bulk_parts = df.write_json()
```

返回值为：

- `json_text: str`
- `bulk_parts: list[tuple[str, memoryview, str, str]]`

每个 bulk tuple 包含：

- `uri`
- `payload`
- `media_type`
- `transfer_syntax_uid`

### C++ write example

```cpp
#include <dicom.h>

auto file = dicom::read_file("sample.dcm");
dicom::JsonWriteResult out = file->write_json();

std::string json_text = std::move(out.json);
for (const auto& part : out.bulk_parts) {
    auto bytes = part.bytes();
    // part.uri
    // part.media_type
    // part.transfer_syntax_uid
}
```

## JSON write options

`JsonWriteOptions` 定义在公开头文件中，在 Python 中通过 keyword
argument 暴露。

### `include_group_0002`

- 默认值：`false`
- 含义：是否在 JSON 输出中包含 file meta group `0002`

默认的 DICOM JSON / DICOMweb 风格输出会排除 group `0002`。
Group length 元素 `(gggg,0000)` 始终会被排除。

### `bulk_data`

Python 取值：

- `"inline"`
- `"uri"`
- `"omit"`

C++ 取值：

- `JsonBulkDataMode::inline_`
- `JsonBulkDataMode::uri`
- `JsonBulkDataMode::omit`

行为：

- `inline`：可作为 bulk 的值仍以内联 `InlineBinary` 输出
- `uri`：达到或超过 threshold 的值输出为 `BulkDataURI`
- `omit`：属性本身仍保留 `vr`，但不输出 bulk 值本体

### `bulk_data_threshold`

- 默认值：`1024`
- 仅在 `bulk_data="uri"` 时使用

当 `bulk_data="uri"` 时，小于 threshold 的值保持 inline，
达到或超过 threshold 的值变为 `BulkDataURI`。

### `bulk_data_uri_template`

在 `bulk_data="uri"` 时，用于 bulk element 的 URI template。

如果没有单独设置 `pixel_data_uri_template`，`PixelData (7FE0,0010)`
也会使用这个 template。

支持的 placeholder：

- `{study}`
- `{series}`
- `{instance}`
- `{tag}`

`{tag}` 的展开规则：

- top-level element：`7FE00010`
- 嵌套 sequence element：如 `22002200.0.12340012` 这样的 dotted tag path

示例：

```python
json_text, bulk_parts = df.write_json(
    bulk_data="uri",
    bulk_data_uri_template="/dicomweb/studies/{study}/series/{series}/instances/{instance}/bulk/{tag}",
)
```

### `pixel_data_uri_template`

这是 `PixelData (7FE0,0010)` 的专用 override。

典型用法：

```python
json_text, bulk_parts = df.write_json(
    bulk_data="uri",
    bulk_data_uri_template="/dicomweb/studies/{study}/series/{series}/instances/{instance}/bulk/{tag}",
    pixel_data_uri_template="/dicomweb/studies/{study}/series/{series}/instances/{instance}/frames",
)
```

当服务器把面向 frame 的 pixel route 与其他 bulk data route 分开暴露时，
通常会这样使用。如果不设置这个 override，`PixelData` 也会回退到
`bulk_data_uri_template`。

### Write `charset_errors`

Python 取值：

- `"strict"`
- `"replace_fffd"`
- `"replace_hex_escape"`

C++ 取值：

- `CharsetDecodeErrorPolicy::strict`
- `CharsetDecodeErrorPolicy::replace_fffd`
- `CharsetDecodeErrorPolicy::replace_hex_escape`

它控制生成 JSON 文本时的文本 decode 处理方式。

## PixelData bulk behavior

### Native PixelData

- JSON 中只保留一个 `BulkDataURI`
- native multi-frame bulk 也保持为一个 aggregate bulk part

### Encapsulated PixelData

- JSON 中仍然只保留一个 base `BulkDataURI`
- `bulk_parts` 会按 frame 返回
- frame URI 取决于所选 base：
  - `/.../frames` -> `/.../frames/1`, `/.../frames/2`, ...
  - generic base URI -> `/.../bulk/7FE00010/frames/1`, ...

这样既能让 JSON 保持紧凑，也能给调用方提供组装 multipart 响应或
frame 响应所需的逐帧 payload 列表。

## 读取 JSON

### Python read example

```python
import dicomsdl as dicom

items = dicom.read_json(json_text)

for df, refs in items:
    ...
```

### C++ read example

```cpp
#include <dicom.h>

dicom::JsonReadResult result = dicom::read_json(
    reinterpret_cast<const std::uint8_t*>(json_bytes.data()),
    json_bytes.size());

for (auto& item : result.items) {
    auto& file = *item.file;
    auto& refs = item.pending_bulk_data;
    (void)file;
    (void)refs;
}
```

reader 总是返回一个 collection，因为 DICOM JSON 可能是：

- 一个 dataset object
- 一个 dataset object 数组

如果 JSON 是单个 top-level object，那么结果列表长度为 `1`。

## JSON read options

### Read `charset_errors`

Python 取值：

- `"strict"`
- `"replace_qmark"`
- `"replace_unicode_escape"`

C++ 取值：

- `CharsetEncodeErrorPolicy::strict`
- `CharsetEncodeErrorPolicy::replace_qmark`
- `CharsetEncodeErrorPolicy::replace_unicode_escape`

该策略会在后续需要把从 UTF-8 JSON 读入的文本转换回 raw DICOM bytes 时
使用，例如 `value_span()`、`write_file(...)`、`set_bulk_data(...)` 等 API。

## bulk 下载流程

典型的 Python 流程：

```python
items = dicom.read_json(json_text)

for df, refs in items:
    for ref in refs:
        payload = download(ref.uri)
        df.set_bulk_data(ref, payload)
```

典型的 C++ 流程：

```cpp
for (auto& item : result.items) {
    for (const auto& ref : item.pending_bulk_data) {
        std::vector<std::uint8_t> payload = download(ref.uri);
        item.file->set_bulk_data(ref, payload);
    }
}
```

`JsonBulkRef` 包含：

- `kind`
- `path`
- `frame_index`
- `uri`
- `media_type`
- `transfer_syntax_uid`
- `vr`

## 读取时的 URI 保留规则

JSON reader 会有意保持保守。

它会保留已经可以直接 dereference 的 URI，例如：

- `.../frames/1`
- `.../frames/1,2,3`
- `https://example.test/instances/1?sig=...` 这类 presigned URL、
  带签名/令牌的下载 URL，或 opaque absolute URL
- `https://example.test/studies/s/series/r/instances/i/bulk/7FE00010?sig=...`
  这类 presigned 或带令牌的 generic pixel URL

只有当 URI 形状本身明确表达了 frame route 时，才会合成 frame URL，例如：

- `.../frames`
- 不带签名/令牌 suffix 的 plain generic base URI，例如 `.../bulk/7FE00010`

这对 presigned URL 或带签名/令牌的下载 URL 很重要：如果把 `/frames/{n}`
追加到已经签名过的 opaque URL 上，path 会发生变化，通常就无法再正确
dereference，因此这些 URL 会保持不变。

## `set_bulk_data(...)` behavior

`set_bulk_data(...)` 支持两个重要场景：

- frame ref：把一个 encoded frame 复制到 encapsulated `PixelData` slot 中
- opaque encapsulated element ref：接收整个 encapsulated `PixelData`
  value field，并将其重建为可写的内部 pixel sequence

这意味着 opaque presigned `BulkDataURI` 或带签名/令牌的 `BulkDataURI`
也能参与常规流程：

1. `read_json(...)` 把 presigned 或带签名/令牌的下载 URL 原样保留为一个
   `element` ref
2. 调用方从该 URL 下载 payload bytes
3. `set_bulk_data(ref, payload)` 会根据下载得到的 value field 重建
   encapsulated `PixelData`

## transfer syntax 相关说明

`JsonBulkPart.transfer_syntax_uid` 和 `JsonBulkRef.transfer_syntax_uid`
会在存在 file meta `TransferSyntaxUID (0002,0010)` 时用该值填充。
如果没有这项信息，reader 会保持保守，不会仅凭 metadata 去猜测
encapsulated frame layout。

## 输入规则

- JSON 输入必须是 UTF-8 文本
- Python 可以接受 `str` 或 bytes-like 输入
- 空输入会报错
- top-level 输入必须是 JSON object 或 array

## 相关文档

- [文件输入/输出](file_io.md)
- [Python 数据集指南](python_dataset_guide.md)
- [Python API Reference](../reference/python_reference.md)
- [DicomFile Reference](../reference/dicomfile_reference.md)
- [DataSet Reference](../reference/dataset_reference.md)
