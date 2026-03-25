# 故障排除

当首次构建、读取、解码或写入失败，并且您需要尽快定位可能原因时，请使用本页。

## 常见故障模式

- 编译前wheel构建失败：
检查Python、`pip`、`cmake`、编译器工具链和活动虚拟环境
- 部分加载的文件上会引发稍后标签突变：
首先加载更多文件，或避免改变尚未解析的数据元素
- `decode_into()` 报告数组 shape、dtype 或缓冲区大小不匹配：
重新检查行、列、每像素样本、帧数和输出项目大小
- 字符集重写失败或发生替换：
检查声明的目标字符集和编码错误策略
- 标签/路径查找无法解析：
确认关键字拼写或点路径形式

## 字符集声明修复

仅当存储的文本字节已经正确，但 `(0008,0005) Specific Character Set` 丢失或错误时才使用此路径。在这种情况下，即使底层字节正常，`to_utf8_string()` 或 `to_person_name()` 也可能会失败。

请勿将此路径用作正常转码工作流程。如果需要将文本重写为不同的字符集，请改用 `set_specific_charset()`。

**C++**

```cpp
#include <dicom.h>
#include <iostream>

using namespace dicom::literals;

dicom::DicomFile file;
auto& study = file.add_dataelement("StudyDescription"_tag, dicom::VR::LO);

// 这些字节已经是 UTF-8，但数据集忘记声明这一事实。
study.from_string_view("심장 MRI");

if (!study.to_utf8_string()) {
    std::cout << "decode failed before declaration repair\n";
}

// 仅修复声明。现有字节保持不变。
file.set_declared_specific_charset(dicom::SpecificCharacterSet::ISO_IR_192);

if (auto utf8 = study.to_utf8_string()) {
    std::cout << *utf8 << '\n';
}
```

**Python**

```python
import dicomsdl as dicom

df = dicom.DicomFile()
study = df.dataset.add_dataelement(dicom.Tag("StudyDescription"), dicom.VR.LO)

# 这些字节已经是 UTF-8，但数据集忘记声明这一事实。
study.from_string_view("심장 MRI")

print(study.to_utf8_string())

# 仅修复声明。现有字节保持不变。
df.set_declared_specific_charset("ISO_IR 192")

print(study.to_utf8_string())
```

## 接下来看哪里

- 读取/解码失败：[错误处理](error_handling.md)
- 字符集文本和 PN 概述：[字符集和人名](charset_and_person_name.md)
- 嵌套路径问题：[序列和路径](sequence_and_paths.md)
- 像素编码问题：[像素编码约束](../reference/pixel_encode_constraints.md)
- 确切的故障类别：[错误模型](../reference/error_model.md)
