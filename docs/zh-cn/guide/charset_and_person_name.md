# 字符集和人名

当文本 VR 或 `PN` 值取决于 `SpecificCharacterSet` 并且您需要解码的 UTF-8 或结构化名称组件时，请使用 `to_utf8_string()` / `to_person_name()`。仅当您故意想要在正常 VR 修剪后存储字节而不进行字符集解码时，才使用 `to_string_view()`。当您需要字符集感知写入时，请使用 `from_utf8_view()` / `from_person_name()`。当您想要将当前数据集子树规范化或转码为新的字符集时，请使用 `set_specific_charset()`。如果您需要修复已存储字节的丢失或错误声明，请参阅[故障排除](troubleshooting.md)。

范围说明：下面的大多数读/写助手都是 `DataElement` 方法。重写或重新声明 `(0008,0005)` 的字符集突变 API 存在于 `DataSet` / `DicomFile` 上。

## 关键字符集和 PN API

**C++**

`DataElement` 方法

- `to_string_view()` / `to_string_views()`
- 读取修剪后的原始存储字节，无需字符集解码。
- `to_utf8_string()` / `to_utf8_strings()`
- 在字符集解码后将文本 VR 读取为拥有的 UTF-8。
- `to_person_name()` / `to_person_names()`
- 将 `PN` 值解析为字母、表意和语音组。
- `from_utf8_view()` / `from_utf8_views()`
- 将 UTF-8 文本编码为当前在所属数据集上声明的字符集。
- `from_person_name()` / `from_person_names()`
- 将结构化 `PersonName` 值序列化为 `PN` 元素。

`DataSet` / `DicomFile` 方法

- `set_specific_charset()`
- 将现有文本字节转码为新的字符集并一致更新 `(0008,0005)`。

`辅助类型`

- `PersonName` / `PersonNameGroup`
- 用于构建或检查 `PN` 值的帮助程序类型，无需手动 `^` 和 `=` 字符串处理。

**Python**

`DataElement` 方法

- `to_string_view()` / `to_string_views()`
- 读取修剪后的原始存储文本，无需字符集解码。
- `to_utf8_string()` / `to_utf8_strings()`
- 将文本 VR 读取为解码的 UTF-8 字符串。使用`return_replaced=True`，您还可以查看解码回退是否替换了字节。
- `to_person_name()` / `to_person_names()`
- 将 `PN` 值解析为具有字母、表意和语音组的 `PersonName` 对象。
- `from_utf8_view()` / `from_utf8_views()`
- 将 Python `str` 数据编码到数据集声明的字符集中。使用 `return_replaced=True`，您可以检查替换行为。
- `from_person_name()` / `from_person_names()`
- 将 `PersonName` 对象序列化为 `PN` 元素。

`DataSet` / `DicomFile` 方法

- `set_specific_charset()`
- 将现有文本值转码为新的字符集并一致地重写 `(0008,0005)`。

`辅助类型`

- `PersonName(...)` / `PersonNameGroup(...)`
- 直接从 Python 字符串或元组构造结构化 `PN` 值。

## 相关 DICOM 标准部分

- `Specific Character Set` 属性本身属于 [DICOM PS3.3 第 C.12 节，通用模块](https://dicom.nema.org/medical/dicom/current/output/chtml/part03/sect_C.12.html) 中的 SOP 通用模块。
- 字符库选择、替换和 ISO/IEC 2022 代码扩展行为在 [DICOM PS3.5 第 6 章，值编码](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/chapter_6.html) 中定义，特别是第 6.1 节和第 6.1.2.4 节到 6.1.2.5 节。
- `PN` 规则在 [DICOM PS3.5 第 6.2 节，值表示](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/sect_6.2.html) 中定义，特别是第 6.2.1 节，人名 (PN) 值表示。
- 日语、韩语、Unicode UTF-8、GB18030 和 GBK 的语言特定示例位于信息丰富的 [DICOM PS3.5 附件 H](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/chapter_H.html)、[附件I](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/chapter_I.html)，和[附件 J](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/chapter_J.html)。

## C++

### 将原始存储文本与解码后的 UTF-8 进行比较

```cpp
#include <dicom.h>
#include <iostream>

using namespace dicom::literals;

auto file = dicom::read_file("patient_names.dcm");
const auto& patient_name = file->dataset()["PatientName"_tag];

// to_string_view() 仅给出正常 VR 修剪后存储的文本字节。
// 这里没有发生 SpecificCharacterSet 解码。
if (auto raw = patient_name.to_string_view()) {
    std::cout << "raw: " << *raw << '\n';
}

// to_utf8_string() 根据数据集声明的 SpecificCharacterSet 进行解码。
if (auto utf8 = patient_name.to_utf8_string()) {
    std::cout << "utf8: " << *utf8 << '\n';
}

// to_person_name() 更进一步，解析 PN 组和组件。
if (auto parsed = patient_name.to_person_name()) {
    if (parsed->alphabetic) {
        std::cout << parsed->alphabetic->family_name() << '\n';
        std::cout << parsed->alphabetic->given_name() << '\n';
    }
}
```

当第一个 `PatientName` 值为 `Hong^Gildong=洪^吉洞=홍^길동` 时的输出示例：

```text
raw: Hong^Gildong=洪^吉洞=홍^길동
utf8: Hong^Gildong=洪^吉洞=홍^길동
Hong
Gildong
```

### 构建并存储结构化的 PersonName

```cpp
#include <dicom.h>
#include <iostream>

using namespace dicom::literals;

dicom::DicomFile file;
file.set_specific_charset(dicom::SpecificCharacterSet::ISO_IR_192);

dicom::PersonName name;
name.alphabetic = dicom::PersonNameGroup{{"Hong", "Gildong", "", "", ""}};
name.ideographic = dicom::PersonNameGroup{{"洪", "吉洞", "", "", ""}};
name.phonetic = dicom::PersonNameGroup{{"홍", "길동", "", "", ""}};

auto& patient_name = file.add_dataelement("PatientName"_tag, dicom::VR::PN);
if (!patient_name.from_person_name(name)) {
    // from_person_name() 也报告正常分配失败，错误。
}

if (auto parsed = patient_name.to_person_name()) {
    std::cout << parsed->alphabetic->family_name() << '\n';
    std::cout << parsed->ideographic->family_name() << '\n';
    std::cout << parsed->phonetic->family_name() << '\n';
}
```

预期输出：

```text
Hong
洪
홍
```

### 将现有文本值转码为新的字符集

```cpp
#include <dicom.h>
#include <iostream>

using namespace dicom::literals;

auto file = dicom::read_file("utf8_names.dcm");
bool replaced = false;

// set_specific_charset() 遍历数据集子树，重写文本 VR 值，
// 并将 (0008,0005) 更新为新声明。该政策保留了
// 转码移动，同时为字符留下可见的痕迹
// 目标字符集不能直接表示。
file->set_specific_charset(
    dicom::SpecificCharacterSet::ISO_IR_100,
    dicom::CharsetEncodeErrorPolicy::replace_unicode_escape,
    &replaced);

// 重写的存储字节现在是纯 ASCII 文本，因此 to_string_view()
// 和 to_utf8_string() 一样，这里也会显示相同的可见 `(U+XXXX)` 替换文本。
if (auto raw_name = file->dataset()["PatientName"_tag].to_string_view()) {
    std::cout << *raw_name << '\n';
}
std::cout << std::boolalpha << replaced << '\n';
```

`utf8_names.dcm` 包含 `홍길동` 时的输出示例：

```text
(U+D64D)(U+AE38)(U+B3D9)
true
```

### 显式处理声明和转码失败

```cpp
#include <dicom.h>
#include <diagnostics.h>
#include <iostream>

using namespace dicom::literals;

try {
    dicom::DicomFile file;
    file.set_specific_charset(dicom::SpecificCharacterSet::ISO_IR_192);

    auto& patient_name = file.add_dataelement("PatientName"_tag, dicom::VR::PN);
    if (!patient_name.from_utf8_view("홍길동")) {
        std::cerr << "initial UTF-8 assignment failed\n";
    }

    // set_specific_charset() 与 from_utf8_view() 不同：
    // 数据集范围的声明/转码问题抛出而不是返回 false。
    file.set_specific_charset(
        dicom::SpecificCharacterSet::ISO_IR_100,
        dicom::CharsetEncodeErrorPolicy::strict);
} catch (const dicom::diag::DicomException& ex) {
    std::cerr << ex.what() << '\n';
}
```

＃＃ Python

### 将原始存储文本与解码后的 UTF-8 进行比较

```python
import dicomsdl as dicom

df = dicom.read_file("patient_names.dcm")
elem = df.dataset["PatientName"]

# to_string_view() 仅在正常 VR 修剪后返回存储的文本。
# 这里没有发生 SpecificCharacterSet 解码。
raw = elem.to_string_view()

# to_utf8_string() 返回解码后的 Python str 或 None。
text, replaced = elem.to_utf8_string(return_replaced=True)

# to_person_name() 返回结构化的 PersonName 或 None。
name = elem.to_person_name()
if name is not None and name.alphabetic is not None:
    print(name.alphabetic.family_name)
    print(name.alphabetic.given_name)
```

当第一个 `PatientName` 值为 `Hong^Gildong=洪^吉洞=홍^길동` 时的输出示例：

```text
Hong
Gildong
```

### 构建并存储结构化的 PersonName

```python
import dicomsdl as dicom

df = dicom.DicomFile()
df.set_specific_charset("ISO_IR 192")

pn = dicom.PersonName(
    alphabetic=("Hong", "Gildong"),
    ideographic=("洪", "吉洞"),
    phonetic=("홍", "길동"),
)

patient_name = df.dataset.add_dataelement(dicom.Tag("PatientName"), dicom.VR.PN)
ok = patient_name.from_person_name(pn)

# 同一个 PersonName 对象也可以配合数据集属性赋值语法糖使用。
df.PatientName = pn

value = df.PatientName
print(value.alphabetic.family_name)
print(value.ideographic.family_name)
print(value.phonetic.family_name)
```

预期输出：

```text
Hong
洪
홍
```

### 对现有文本值进行转码并检查替换

```python
import dicomsdl as dicom

df = dicom.read_file("utf8_names.dcm")

# 可见的后备通常比严格的失败更容易处理。
# 生产清理过程，因为转码完成并且替换是
# 在结果文本中仍然很明显。
replaced = df.set_specific_charset(
    "ISO_IR 100",
    errors="replace_unicode_escape",
    return_replaced=True,
)
print(df.get_dataelement("PatientName").to_string_view())
print(replaced)
```

`utf8_names.dcm` 包含 `홍길동` 时的预期输出：

```text
(U+D64D)(U+AE38)(U+B3D9)
True
```

### 显式处理声明和转码失败

```python
import dicomsdl as dicom

df = dicom.DicomFile()

try:
    df.set_specific_charset("ISO_IR 192")
    patient_name = df.dataset.add_dataelement(dicom.Tag("PatientName"), dicom.VR.PN)
    ok = patient_name.from_utf8_view("홍길동")
    print(ok)

    # 如果无法完成请求的转码，则引发 set_specific_charset()
    # 根据所选的错误策略。
    df.set_specific_charset("ISO_IR 100", errors="strict")
except (TypeError, ValueError) as exc:
    # 字符集参数形状无效或策略文本无效。
    print(exc)
except RuntimeError as exc:
    # 基础声明或转码步骤失败。
    print(exc)
```

## `set_specific_charset()` 策略选项

第一个参数选择目标字符集。第二个参数选择如何处理目标字符集无法表示的字符。可选的第三个输出报告是否实际发生了任何替换，这主要对有损模式有用。

当每个文本值都可以用目标字符集表示时，所有策略都会生成相同的转码数据集并报告 `replaced == false`。仅当某些现有文本无法以请求的目标字符集表示时，差异才有意义。

策略名称直接映射到两个 API：

- C++：`dicom::CharsetEncodeErrorPolicy::strict`、`::replace_qmark`、`::replace_unicode_escape`
- Python：`errors="strict"`、`"replace_qmark"`、`"replace_unicode_escape"`

例如，如果源文本为 `홍길동`，目标字符集为 `ISO_IR 100`，则该目标字符集无法直接表示韩语字符。然后政策就会出现这样的分歧：

|对比点| `strict` | `replace_qmark` | `replace_unicode_escape` |
| --- | --- | --- | --- |
|如果某些文本无法表示 | `set_specific_charset()` 抛出/升高并停止。 |转码成功并替换为`?`。 |转码成功并替换可见的 `(U+XXXX)` 文本。 |
| `홍길동 -> ISO_IR 100` 的结果示例 |由于调用失败，因此不会生成转码文本。 | `???` | `(U+D64D)(U+AE38)(U+B3D9)` |
|数据集提交 |没有变化。 |字符集已更新，文本 VR 已用 `?` 重写。 |字符集已更新，文本 VR 已用 `(U+XXXX)` 替换文本重写。 |
| `replaced` 输出 |不适用，因为呼叫失败。 |当至少发生一次替换时为 `true`。 |当至少发生一次替换时为 `true`。 |

可选的 `replaced` 输出对于上述有损模式最有用：

- 在 C++ 中，传递 `bool* out_replaced`。
- 在Python中，使用`return_replaced=True`。
- 当转码准确时，它保持 `false`，仅当替换策略实际上必须替换字符时，才会翻转到 `true`。

转码在目标编码之前还有一个源解码阶段。如果当前数据集已包含无法在当前声明下解码的字节，则同样的策略名称也适用。

例如，如果当前声明为 `ISO_IR 192`，但存储的原始文本值包含无效的 UTF-8 字节 `b"\xFF"`，则源解码阶段会出现如下情况：

|对比点| `strict` | `replace_qmark` | `replace_unicode_escape` |
| --- | --- | --- | --- |
|如果当前存储的字节已经不可解码 | `set_specific_charset()` 抛出/升高并停止。 |转码继续并用 `?` 替换不可解码的源字节范围。 |转码继续并替换可见的字节转义。 |
|原始字节 `b"\xFF"` 的替换示例 |由于调用失败，因此不会生成转码文本。 | `?` | `(0xFF)` |
|为什么这与目标编码后备不同？没有恢复 Unicode 文本，因此转码无法继续。 |没有恢复 Unicode 代码点，因此回退只是 `?`。 |没有恢复 Unicode 代码点，因此后备是 `(0xNN)` 字节转义而不是 `(U+XXXX)`。 |

## `to_utf8_string()` 解码策略选项

这些策略控制当存储的字节无法在当前声明的字符集下干净地解码时会发生什么。

策略名称直接映射到两个 API：

- C++：`dicom::CharsetDecodeErrorPolicy::strict`、`::replace_fffd`、`::replace_hex_escape`
- Python：`errors="strict"`、`"replace_fffd"`、`"replace_hex_escape"`

例如，如果数据集声明 `ISO 2022 IR 100` 但存储的原始字节对于该解码路径无效，例如 `b"\x1b%GA"`，则 `to_utf8_string()` 会出现如下情况：

|对比点| `strict` | `replace_fffd` | `replace_hex_escape` |
| --- | --- | --- | --- |
|如果存储的字节不能被干净地解码 | `to_utf8_string()` 失败。 |解码成功并替换字符。 |解码成功并出现可见的字节转义。 |
| `b"\x1b%GA"` 的结果示例 |不产生解码文本。 | `�` | `(0x1B)(0x25)(0x47)(0x41)` |
|返回值 | C++ 中的 `nullopt`，Python 中的 `None` |解码的 UTF-8 文本 |解码的 UTF-8 文本 |
| `replaced` 输出 | `false` 因为没有返回值 | `true` 当至少发生一次替换时 | `true` 当至少发生一次替换时 |

## `from_utf8_view()` 编码策略选项

这些策略控制当输入 UTF-8 文本无法用数据集当前声明的字符集表示时会发生什么情况。 `from_utf8_view()` 是一个返回值 API，因此与 `set_specific_charset()` 不同，它使用 `false` 报告普通编码失败，而不是抛出/引发。

策略名称直接映射到两个 API：

- C++：`dicom::CharsetEncodeErrorPolicy::strict`、`::replace_qmark`、`::replace_unicode_escape`
- Python：`errors="strict"`、`"replace_qmark"`、`"replace_unicode_escape"`

例如，如果数据集声明为 `ISO_IR 100`，输入文本为 `홍길동`，则声明的字符集无法直接表示韩文字符。 `from_utf8_view()` 然后像这样发散：

|对比点| `strict` | `replace_qmark` | `replace_unicode_escape` |
| --- | --- | --- | --- |
|如果输入文本无法用声明的字符集表示 |调用失败并且不存储任何新内容。 |调用成功并替换为`?`。 |调用成功并替换可见的 `(U+XXXX)` 文本。 |
| `홍길동 -> ISO_IR 100` 的存储文本示例 |不生成编码文本。 | `???` | `(U+D64D)(U+AE38)(U+B3D9)` |
|返回值 | `false` | `true` | `true` |
| `replaced` 输出 | `false` 因为写入没有成功 | `true` 当至少发生一次替换时 | `true` 当至少发生一次替换时 |

## 失败模型

**C++**

|应用程序接口 |失败表格 |典型原因 |
| --- | --- | --- |
| `to_utf8_string()` / `to_person_name()` |空 `std::optional` | VR错误，字符集解码失败，或者解码后无法解析`PN`语法。 |
| `from_utf8_view()` / `from_person_name()` | `false` | VR 错误、输入不是有效的 UTF-8、声明的字符集无法表示所选策略下的文本，或者由于 DICOM 原因分配失败。 |
| `set_specific_charset()` | `dicom::diag::DicomException` |无效的目标字符集声明、不受支持的声明组合或数据集范围的转码失败。 |

**Python**

|应用程序接口 |失败表格 |典型原因 |
| --- | --- | --- |
| `to_utf8_string()` / `to_person_name()` | `None` 或 `(None, replaced)` | VR错误，字符集解码失败，或者解码后无法解析`PN`语法。无效的 `errors=` 值会引发 `ValueError`。 |
| `from_utf8_view()` / `from_person_name()` | `False` 或 `(False, replaced)` |目标 VR 不兼容，声明的字符集无法表示所选策略下的文本，或者分配失败。错误的 Python 参数类型会引发 `TypeError`。 |
| `set_specific_charset()` | `TypeError`、`ValueError`、`RuntimeError` |字符集参数形状无效、字符集术语未知或底层 C++ 转码步骤失败。 |
| `PersonNameGroup.component(index)` | `IndexError` |组件索引位于 `[0, 4]` 之外。 |

## 注释

- `to_string_view()` 和 `to_string_views()` 在 VR 修剪规则后返回原始文本字节。它们不执行字符集解码。将 `to_utf8_string()` 和 `to_utf8_strings()` 用于面向应用程序的文本。
- `to_string_views()` 可以为声明的多字节字符集（例如 ISO 2022 JIS、GBK 或 GB18030）返回 `nullopt` / `None`，因为在解码之前在 `\` 上分割原始字节是不安全的。
- `set_specific_charset()` 重写数据集子树中的文本 VR 值，并将 `(0008,0005)` 同步到新声明。
- `set_specific_charset("ISO_IR 192")` 是新 Unicode 内容的合理正常流起点，因为它在以后的 `from_utf8_view()` 或 `from_person_name()` 写入之前将数据集保留在 UTF-8 声明状态。
- `from_utf8_view()` 和 `from_person_name()` 是普通返回值 API。 `false`表示元素写入未成功。 `set_specific_charset()` 是一个验证/转码 API，并通过抛出/引发来报告失败。
- `PersonName` 最多可承载三组：字母、表意和语音。
- `PersonNameGroup` 按 DICOM 顺序最多携带五个组成部分：姓氏、名字、中间名、前缀和后缀。
- 嵌套序列项数据集从其父项继承有效字符集，除非该项声明其自己的本地 `(0008,0005)`。
- `PersonName` 解析和序列化保留显式的空组和空组件，因此您不需要手动组装 `=` 和 `^` 分隔符来保留这些详细信息。
- 对于新的 Unicode 内容，`ISO_IR 192` 通常是最简单的声明，因为存储的文本是纯 UTF-8，没有 ISO 2022 转义状态管理。
- 如果存储的字节已经正确，但 `(0008,0005)` 丢失或错误，请参阅[故障排除](troubleshooting.md) 了解声明修复路径。
- 当目标是正常的转码或标准化流程时，优先选择 `set_specific_charset()` 而不是将 `(0008,0005)` 作为原始元素进行变异。

## 相关文档

- [核心对象](core_objects.md)
- [Python数据集指南](python_dataset_guide.md)
- [C++ 数据集指南](cpp_dataset_guide.md)
- [错误处理](error_handling.md)
- [疑难解答](troubleshooting.md)
