# 快速开始

## Python
对大多数用户来说，先从 PyPI 安装路径开始即可。
1. 要求: Python 3.9+, `pip`
2. 从 PyPI 安装

```bash
python -m pip install --upgrade pip
pip install "dicomsdl[numpy,pil]"
```

如果当前平台上 `pip` 回退到 source build，请先安装 `cmake`。

```{note}
如果你在服务器上只需要 metadata access、file I/O 或 transcode workflow，
`pip install dicomsdl` 就够了。
```

如果需要 source build、custom wheel 或测试 workflow，请参见 [Build Python From Source](../developer/build_python_from_source.md)。
如果需要平台相关的安装细节，请参见 [Installation](installation.md)。

3. 读取 metadata

```pycon
>>> import dicomsdl as dicom
>>> df = dicom.read_file("sample.dcm")
>>> df.PatientName
PersonName(Doe^Jane)
>>> df.Rows, df.Columns
(512, 512)
```

`DicomFile` 会转发 root `DataSet` 的 access helper，因此在 Python 中，`df.Rows`、`df.PatientName` 这样的普通顶层 keyword 读取通常是最短、也最推荐的 metadata read 路径。
嵌套 leaf lookup 请使用 `df.get_value("Seq.0.Tag")`；如果你需要的是 `DataElement` 元数据而不是类型化值，请使用 `df["Rows"]` / `df.get_dataelement(...)`。
已知 keyword 只是缺失时会返回 `None`，未知 keyword 仍然会抛出 `AttributeError`。
如果你希望 dataset 边界更明确，请使用 `df.dataset`。
`PatientName` 是 `PN`，因此 `df.PatientName` 显示的是 `PersonName(...)` 对象，而不是普通 Python 字符串。
如果你需要对象模型、metadata lookup 规则或完整 decode 流程，请参见 [Core Objects](core_objects.md)、[Python DataSet Guide](python_dataset_guide.md) 和 [Pixel Decode](pixel_decode.md)。

4. 将像素 decode 到 NumPy 数组

```pycon
>>> import dicomsdl as dicom
>>> df = dicom.read_file("sample.dcm")
>>> arr = df.to_array()
>>> arr.shape
(512, 512)
>>> arr.dtype
dtype('uint16')
```

如果需要 decode 选项、frame 选择或输出 layout 控制，请参见 [Pixel Decode](pixel_decode.md)。

5. 使用 Pillow 快速预览图像

```bash
pip install "dicomsdl[numpy,pil]"
```

```python
import dicomsdl as dicom

df = dicom.read_file("sample.dcm")
image = df.to_pil_image(frame=0)
image.show()
```

`to_pil_image()` 是一个范围较窄的 convenience helper，适合快速目视检查。
在分析流水线和可重复处理场景中，更推荐 `to_array()`。`show()` 依赖本地 GUI / viewer，在 headless 环境中可能无法工作。
如果需要 decode 选项或面向数组的 workflow，请参见 [Pixel Decode](pixel_decode.md)。

6. 转码到 `HTJ2KLossless` 并写出新文件

```python
from pathlib import Path

import dicomsdl as dicom

in_path = Path("in.dcm")
out_path = Path("out_htj2k_lossless.dcm")

df = dicom.read_file(in_path)
df.set_transfer_syntax("HTJ2KLossless")
df.write_file(out_path)

print("Input bytes:", in_path.stat().st_size)
print("Output bytes:", out_path.stat().st_size)
```

对于一个代表性文件，输出大致如下：

```text
Input bytes: 525312
Output bytes: 287104
```

这个 file-to-file transcode 路径使用基础的 `pip install dicomsdl` 也能工作。实际的大小变化取决于 source transfer syntax、pixel content 和 metadata。
如果需要 lossy encode 选项、codec 限制或 streaming write 指南，请参见 [Pixel Encode](pixel_encode.md)、[Pixel Encode Constraints](../reference/pixel_encode_constraints.md) 和 [Encode-capable Transfer Syntax Families](../reference/codec_support_matrix.md)。

7. 通过 `memoryview` 访问 `DataElement` 的 value bytes

```python
import dicomsdl as dicom

df = dicom.read_file("sample.dcm")
elem = df["PixelData"]
if elem:
    raw = elem.value_span()  # memoryview
    print("Raw bytes:", raw.nbytes)
    print("Head:", list(raw[:8]))
```

对于一个未压缩的 `512 x 512` `uint16` 图像：

```text
Raw bytes: 524288
Head: [34, 12, 40, 12, 36, 12, 39, 12]
```

前几个字节的内容会因文件而异。这个直接的 `value_span()` view 适用于 native / uncompressed `PixelData`。对于压缩的 encapsulated transfer syntax，`PixelData` 会存成 `PixelSequence`，因此 `elem.value_span()` 为空，这时应使用 `elem.pixel_sequence.frame_encoded_memoryview(0)` 或 `elem.pixel_sequence.frame_encoded_bytes(0)`。
在使用 `raw` 期间请保持 `df` 存活。这个 memoryview 指向的是已加载 DICOM 对象拥有的字节，如果这些字节被替换，它就会失效。
如果需要 raw byte 语义或 encapsulated `PixelData` 的细节，请参见 [DataElement Reference](../reference/dataelement_reference.md) 和 [Pixel Reference](../reference/pixel_reference.md)。

如果需要完整的 decode safety 模型，请参见 [Pixel Decode](pixel_decode.md) 和 [Error Handling](error_handling.md)。

## C++
从仓库 checkout 构建。
要求: `git`、`CMake`、`C++20` 编译器
1. 克隆仓库

```bash
git clone https://github.com/tsangel/dicomsdl.git
cd dicomsdl
```

2. configure 与 build
```bash
cmake -S . -B build -DDICOM_BUILD_EXAMPLES=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

3. 使用示例
```cpp
#include <dicom.h>
#include <diagnostics.h>
#include <iostream>
#include <memory>
using namespace dicom::literals;

int main() {
  auto file = dicom::read_file("sample.dcm");
  auto& ds = file->dataset();

  long rows = ds["Rows"_tag].to_long().value_or(0);
  // 如果“是否存在”本身就重要，就在取值前先判断元素是否存在。
  long cols = 0;
  if (auto& e = ds["Columns"_tag]; e) {
    cols = e.to_long().value_or(0);
  }
  std::cout << "Image size: " << rows << " x " << cols << '\n';
}
```

典型输出如下：

```text
Image size: 512 x 512
```

如果需要更多 C++ API 细节，请参见 [C++ API Overview](../reference/cpp_api.md) 和 [DataSet Reference](../reference/dataset_reference.md)。

4. 用 `ok &= ...` 配合错误检查批量设置
```cpp
#include <dicom.h>
#include <diagnostics.h>
#include <memory>
#include <iostream>
using namespace dicom::literals;

int main() {
  dicom::DataSet ds;
  auto reporter = std::make_shared<dicom::diag::BufferingReporter>(256);
  dicom::diag::set_thread_reporter(reporter);

  bool ok = true;
  ok &= ds.add_dataelement("Rows"_tag, dicom::VR::US).from_long(512);
  ok &= ds.add_dataelement("Columns"_tag, dicom::VR::US).from_long(-1); // 故意失败示例

  if (!ok) {
    for (const auto& msg : reporter->take_messages()) {
      std::cerr << msg << '\n';
    }
  }
  dicom::diag::set_thread_reporter(nullptr);
}
```

上面这个例子故意让 `Columns = -1` 失败，因此输出大致如下。
`VR::US` 只接受 unsigned 值，所以 `Columns = -1` 会触发 range error：

```text
[ERROR] from_long tag=(0028,0011) vr=US reason=value out of range for VR
```

- 完整可运行示例: `examples/batch_assign_with_error_check.cpp`
- `add_dataelement(...)` 返回 `DataElement&`，因此 write helper 以 `.` 链接。
如果需要更广泛的写入模式或失败处理说明，请参见 [C++ DataSet Guide](cpp_dataset_guide.md) 和 [Error Handling](error_handling.md)。
