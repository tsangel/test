# 错误处理

本页汇总了分散在各个重点指南中的错误处理模式。需要快速回答下面两个问题时，可以先看这里：

- 哪些 API 会抛出异常？
- 它们失败时，下一步该怎么办？

## 错误类型

`dicomsdl` 的公共 API 大致有三种失败方式：

- 抛出异常
- 高层 C++ 读取、写入、解码、编码以及数据集范围的字符集变更 API 失败时，通常会抛出 `dicom::diag::DicomException`。
- 在 Python 中，同类运行时失败通常表现为 `RuntimeError`；绑定层的参数校验错误通常表现为 `TypeError`、`ValueError` 或 `IndexError`。
- 返回值失败
- 某些元素级字符集 API 不是异常式 API，而是普通返回值 API。它们会用 `false`、`None` 或空 `optional` 表示失败。
- 部分成功并记录了错误状态
- `read_file(..., keep_on_error=True)` 和 `read_bytes(..., keep_on_error=True)` 即使发生解析错误，仍可能返回 `DicomFile`，但对象会通过 `has_error` 和 `error_message` 标记错误状态。

## 异常处理模式

**C++**

```cpp
try {
    // 高级 dicomsdl 操作
} catch (const dicom::diag::DicomException& ex) {
    // 面向用户的 DICOM、编解码器或文件 I/O 故障
} catch (const std::exception& ex) {
    // 较低级别的合约或平台故障
}
```

**Python**

```python
import dicomsdl as dicom

try:
    # 高级 dicomsdl 操作
    ...
except TypeError as exc:
    # 错误的参数类型或非缓冲区/类似路径的误用
    ...
except ValueError as exc:
    # 无效的文本选项、无效的缓冲区/布局请求、格式错误的合同
    ...
except IndexError as exc:
    # 框架或组件索引超出范围
    ...
except RuntimeError as exc:
    # 底层 C++ 解析、解码、编码、转码或写入失败
    ...
```

## 文件输入/输出

当任何解析问题应立即拒绝文件时，请使用 `keep_on_error=False`。当早期元数据仍然有用时，即使文件后来被证明格式错误，也可以使用 `keep_on_error=True`。

### `keep_on_error=False`：快速失败

- 将此用于导入管道、验证作业或任何格式错误的文件应立即停止的工作流程。
- 将任何异常视为“继续处理此文件不安全”。
- 记录路径和异常文本，然后隔离、跳过或报告文件。

### `keep_on_error=True`：保留已经解析的内容

- 将其用于爬虫、元数据索引、分类工具或修复工具，这些工具仍然可以从早期标签中受益。
- 每次许可读取后，请在信任结果之前检查 `has_error` 和 `error_message`。
- 如果 `has_error` 为 true，则将对象视为部分读取或受污染：
- 只使用您明确想恢复的元数据
- 不要盲目地继续进行像素解码、像素编码或回写流程
- 当您需要完全值得信赖的对象时，修复后严格重新加载
- `keep_on_error` 不是一般的“忽略所有错误”开关。路径/打开失败、无效的 Python 缓冲区合约以及类似的边界错误仍然会立即引发。

### 示例

**C++**

```cpp
#include <dicom.h>
#include <diagnostics.h>
#include <iostream>

try {
    dicom::ReadOptions opts;
    opts.keep_on_error = true;

    auto file = dicom::read_file("in.dcm", opts);
    if (file->has_error()) {
        std::cerr << "partial read: " << file->error_message() << '\n';
        // 只保留您明确想恢复的元数据。
        // 不要继续解码/转码，就好像这是一个干净的文件一样。
    }
} catch (const dicom::diag::DicomException& ex) {
    // 文件打开失败或 keep_on_error 的其他边界故障
    // 不转换为部分返回状态。
    std::cerr << ex.what() << '\n';
}
```

**Python**

```python
import dicomsdl as dicom

df = dicom.read_file("in.dcm", keep_on_error=True)
if df.has_error:
    print("partial read:", df.error_message)
    # 仅使用您尝试检查的已解析元数据。
    # 严格在解码/转码/写入工作流程之前重新加载。
```

### 会抛出异常的文件 I/O API

| API家族| C++ 失败形式 | Python 异常 |典型原因 |
| --- | --- | --- | --- |
| `read_file(...)` |严格读取失败时为`dicom::diag::DicomException`；使用 `keep_on_error=true`，解析失败会在返回的 `DicomFile` 上捕获 | `TypeError`、`RuntimeError` |路径无法打开、严格解析失败或 Python 路径参数不是 `str` / `bytes` / `os.PathLike` |
| `read_bytes(...)` |严格读取失败时为`dicom::diag::DicomException`；使用 `keep_on_error=true`，解析失败会在返回的 `DicomFile` 上捕获 | `TypeError`、`ValueError`、`RuntimeError` |缓冲区不是一维连续字节状数据，`copy=False` 与非字节元素一起使用，或者解析失败 |
| `write_file(...)` | `dicom::diag::DicomException` | `TypeError`、`RuntimeError` |输出路径无效、文件打开/刷新失败、文件元重建失败或数据集无法在当前状态下序列化 |
| `write_bytes(...)` | `dicom::diag::DicomException` | `RuntimeError` |文件元重建失败或当前数据集无法干净地序列化 |
| `write_with_transfer_syntax(...)` | `dicom::diag::DicomException` | `TypeError`、`ValueError`、`RuntimeError` |输出路径无效、传输语法选择无效、编码器上下文/选项与请求不匹配、转码失败或输出写入失败 |

## 像素解码

最安全的解码模式是：

1. 预先创建一个`DecodePlan`
2. 从该计划中分配目的地
3. 在解码调用中重复使用相同的经过验证的计划和目标合同

当解码失败时，通常先从三个方向排查：调用方约定错误、过时的布局假设，或者真实的后端/运行时解码失败。

### 解码失败时怎么办

- 如果在解码开始之前验证失败：
- 检查帧索引、目标大小、连续性和 `DecodeOptions`
- 如果之前的良好计划开始失败：
- 在任何影响像素的元数据更改后重新创建计划和目的地
- 如果运行时解码失败：
- 记录消息并将其视为文件/编解码器问题，而不仅仅是形状问题
- 在 Python 中：
- `TypeError`、`ValueError` 和 `IndexError` 通常表示参数或请求的布局有误
- `RuntimeError` 通常表示底层解码路径本身失败

### 会抛出异常的像素解码 API

| API家族| C++ 失败形式 | Python 异常 |典型原因 |
| --- | --- | --- | --- |
| `create_decode_plan(...)` | `dicom::diag::DicomException` | `RuntimeError` |像素元数据丢失或不一致、显式步幅无效或请求的解码布局溢出 |
| `decode_into(...)` | `dicom::diag::DicomException` | `TypeError`、`ValueError`、`IndexError`、`RuntimeError` |帧索引无效，目标大小或布局错误，计划不再与文件状态匹配，或者解码器/后端失败 |
| `pixel_buffer(...)` | `dicom::diag::DicomException` |不直接暴露|与拥有缓冲区便利路径上的 `decode_into(...)` 相同的底层解码失败 |
| `decode_all_frames_into(...)` | `dicom::diag::DicomException` |由 `decode_into(..., frame=-1)` 和 `to_array(frame=-1)` 覆盖 |目标太小、帧元数据无效或批量解码/后端执行失败 |
| `to_array(...)` |不适用 | `ValueError`、`IndexError`、`RuntimeError` |无效的帧请求、无效的解码选项请求或底层解码失败 |
| `to_array_view(...)` |不适用 | `ValueError`、`IndexError` |无效的帧请求、压缩的源数据或没有兼容的直接原始像素视图 |

## 像素编码

最安全的编码模式是：

1. 在长循环之前验证目标传输语法和选项
2. 当相同的传输语法和选项集重复时更喜欢 `EncoderContext`
3. 当目标只是不同编码的输出文件时，更喜欢 `write_with_transfer_syntax(...)`

### 编码失败时怎么办

- 如果构建 `EncoderContext` 时发生故障：
- 在开始真正的编码循环之前修复传输语法或选项集
- 如果在`set_pixel_data(...)`期间发生故障：
- 首先验证源缓冲区形状、数据类型、连续性和像素元数据假设
- 如果在`set_transfer_syntax(...)`期间发生故障：
- 将其视为当前对象状态的内存中转码失败
- 如果目标只是输出：
- 更喜欢 `write_with_transfer_syntax(...)`，这样失败的转码就不会成为您正常的内存工作流程

### 会抛出异常的像素编码 API

| API家族| C++ 失败形式 | Python 异常 |典型原因 |
| --- | --- | --- | --- |
| `create_encoder_context(...)` / `EncoderContext::configure(...)` | `dicom::diag::DicomException` | `TypeError`、`ValueError`、`RuntimeError` |传输语法无效、选项键/值无效或运行时编码器配置失败 |
| `set_pixel_data(...)` | `dicom::diag::DicomException` | `TypeError`、`ValueError`、`RuntimeError` |源缓冲区类型/形状/布局无效、源字节与声明的布局不一致、编码器选择失败或编码/后端更新失败 |
| `set_transfer_syntax(...)` | `dicom::diag::DicomException` | `TypeError`、`ValueError`、`RuntimeError` |传输语法选择无效、选项/上下文与请求不匹配、或转码/后端路径失败 |
| `write_with_transfer_syntax(...)` | `dicom::diag::DicomException` | `TypeError`、`ValueError`、`RuntimeError` |无效的路径或传输语法文本、无效的选项/上下文、不支持的转码路径、后端编码失败或输出写入失败 |

## 字符集和人名

字符集处理故意混合两种样式：

- 元素级读/写助手大多报告 `None`、空 `optional` 或 `false` 的普通故障
- 数据集范围的字符集变更属于验证/转码操作，失败时会抛出异常

当您决定失败是“这个文本分配失败”还是“整个数据集转码应该停止”时，这种区别很重要。

### 字符集工作失败时该怎么办

- 对于 `to_utf8_string()` / `to_person_name()`：
- 将空 `optional` 或 `None` 视为“解码/解析未产生可用值”
- 当您想要尽力而为的文本而不是严格的失败时，选择替换策略
- 对于 `from_utf8_view()` / `from_person_name()`：
- 将 `false` 视为“在当前字符集/策略下此写入未成功”
- 当有损替换可以接受并且您想知道它是否发生时，在 Python 中使用 `return_replaced=True` 或在 C++ 中使用 `bool* out_replaced`
- 对于 Python 元素级助手：
- 请记住，在到达正常返回值路径之前，无效的 `errors=` 文本仍然会引发 `ValueError`
- 对于 `set_specific_charset()`：
- 使用 `strict` 进行验证或快速失败清理
- 当您希望完成转码并保留可见的 `(U+XXXX)` 替换文本时，请使用 `replace_unicode_escape`
- 如果当前数据集可能已包含错误声明的原始字节，请使用故障排除流程，而不是将正常转码视为声明修复

### 会抛出异常的字符集 API

| API家族| C++ 失败形式 | Python 异常 |典型原因 |
| --- | --- | --- | --- |
| `set_specific_charset(...)` | `dicom::diag::DicomException` | `TypeError`、`ValueError`、`RuntimeError` |字符集声明文本无效、策略文本无效、源文本无法在所选策略下转码或数据集范围的字符集突变失败 |
| `set_declared_specific_charset(...)` | `dicom::diag::DicomException` | `TypeError`、`ValueError`、`RuntimeError` |声明参数无效或`(0008,0005)`无法一致更新；主要用于修复/故障排除流程 |

### 不会因普通内容失败而引发的字符集 API

| API家族| C++ 失败形式 | Python 失败形式 |典型含义|
| --- | --- | --- | --- |
| `to_utf8_string()` / `to_utf8_strings()` |空 `std::optional` | `None` 或 `(None, replaced)` | VR 错误、字符集解码失败或未生成可用的解码文本 |
| `to_person_name()` / `to_person_names()` |空 `std::optional` | `None` 或 `(None, replaced)` | VR错误、字符集解码失败或解码后PN解析失败 |
| `from_utf8_view()` / `from_utf8_views()` | `false` | `False` 或 `(False, replaced)` |当前字符集和错误策略下元素写入未成功 |
| `from_person_name()` / `from_person_names()` | `false` | `False` 或 `(False, replaced)` |在当前字符集和错误策略下PN写入未成功|

## 我应该从哪种策略开始？

- 格式错误的文件应立即停止工作流程
- 使用严格的`read_file(...)` / `read_bytes(...)`
- 我想从格式错误的文件中恢复元数据
- 使用 `keep_on_error=True`，然后始终检查 `has_error` 和 `error_message`
- 我想要调用者管理的解码缓冲区或显式输出步幅
- 使用`create_decode_plan(...)`加`decode_into(...)`
- 我首先想要最简单的解码路径
- 在Python中使用`to_array()`或在C++中使用`pixel_buffer()`
- 我正在使用相同的编码配置编写许多输出
- 构建一个 `EncoderContext`
- 我只想要不同的输出传输语法
- 优先选择`write_with_transfer_syntax(...)`
- 我正在跨数据集对文本值进行变异或转码
- 使用`set_specific_charset(...)`
- 我正在读取或写入一个文本元素，并且想要普通的是/否失败
- 使用 `to_utf8_string()` / `from_utf8_view()` 及其 PN 变体

## 相关文档

- [文件I/O](file_io.md)
- [像素解码](pixel_decode.md)
- [像素编码](pixel_encode.md)
- [字符集和人名](charset_and_person_name.md)
- [疑难解答](troubleshooting.md)
- [错误模型](../reference/error_model.md)
