# 像素编码

当您已经有以原生格式存储、准备编码的像素时，请使用 `set_pixel_data()`。当当前 `DicomFile` 已经包含像素数据，并且您想在内存中对其转码时，请使用 `set_transfer_syntax()`。当您希望直接以另一种传输语法输出、而不先修改源对象时，请使用 `write_with_transfer_syntax()`。如果同一套传输语法和选项会在多次调用中重复使用，或者您想在开始较长的编码循环前先验证配置，请创建 `EncoderContext`。

## 关键编码 API

**C++**

- `set_pixel_data(...)`
- 替换来自原生源缓冲区的 Pixel Data，并由您通过 `pixel::ConstPixelSpan` 显式描述其布局。
- `create_encoder_context(...)` + `set_pixel_data(...)` / `set_transfer_syntax(...)`
- 在重复编码或转码循环之外，保留并复用一套已配置好的传输语法和选项。
- `write_with_transfer_syntax(...)`
- 将不同的传输语法直接写入文件或流，而不改变内存中的 `DicomFile`。

**Python**

- `set_pixel_data(...)`
- 替换 C 连续 NumPy 数组或其他连续数字缓冲区中的像素数据。
- `create_encoder_context(...)` + `set_pixel_data(...)` / `set_transfer_syntax(...)`
- 先解析并验证一个 Python `options` 对象，然后在重复调用中复用生成的上下文。
- `write_with_transfer_syntax(...)`
- 将不同的传输语法直接写入文件，而无需首先改变源对象。
- `set_transfer_syntax(...)`
- 当您希望后续继续从同一个对象读取或写入，并使用新的传输语法时，在内存中对当前 `DicomFile` 进行转码。

## 相关 DICOM 标准部分

- 必须与编码数据保持一致的像素元数据在 [DICOM PS3.3 第 C.7.6.3 节，图像像素模块](https://dicom.nema.org/medical/dicom/current/output/chtml/part03/sect_C.7.6.3.html) 中定义。
- 原生与封装像素数据编码和编解码器特定的 8.2.x 规则在 [DICOM PS3.5 第 8 章，像素、叠加和波形数据编码](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/chapter_8.html) 和 [第 8.2 节，原生或封装格式编码](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/sect_8.2.html)。
- 封装的传输语法和片段规则在 [DICOM PS3.5 第 A.4 节，编码像素数据封装的传输语法](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/sect_A.4.html) 中定义。
- 在基于文件的编码和转码工作流程中，生成的传输语法 UID 在 [DICOM PS3.10 第 7 章，DICOM 文件格式](https://dicom.nema.org/medical/dicom/current/output/chtml/part10/chapter_7.html) 定义的文件元信息中携带。

## C++

### 在调用 `set_pixel_data()` 之前明确描述源像素

```cpp
#include <cstdint>
#include <dicom.h>
#include <random>
#include <span>
#include <vector>

using namespace dicom::literals;

auto file = dicom::read_file("sample.dcm");

const std::uint32_t rows = 256;
const std::uint32_t cols = 256;
const std::uint32_t frames = 1;

std::vector<std::uint16_t> pixels(rows * cols * frames);
std::mt19937 rng(0);
std::uniform_int_distribution<int> dist(0, 4095);
for (auto& px : pixels) {
    px = static_cast<std::uint16_t>(dist(rng));
}

const dicom::pixel::ConstPixelSpan source{
    .layout = dicom::pixel::PixelLayout{
        .data_type = dicom::pixel::DataType::u16,
        .photometric = dicom::pixel::Photometric::monochrome2,
        .planar = dicom::pixel::Planar::interleaved,
        .reserved = 0,
        .rows = rows,
        .cols = cols,
        .frames = frames,
        .samples_per_pixel = 1,
        .bits_stored = 12,
        .row_stride = cols * sizeof(std::uint16_t),
        .frame_stride = rows * cols * sizeof(std::uint16_t),
    },
    .bytes = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(pixels.data()),
        pixels.size() * sizeof(std::uint16_t)),
};

// set_pixel_data() 使用上面的布局来读取本机源缓冲区并
// 重写 DicomFile 上匹配的图像像素元数据。
file->set_pixel_data("RLELossless"_uid, source);
```

### 在重复写入循环之外保留一个预配置的上下文

```cpp
#include <array>
#include <dicom.h>
#include <diagnostics.h>
#include <iostream>
#include <span>

using namespace dicom::literals;

auto file = dicom::read_file("sample.dcm");

const std::array<dicom::pixel::CodecOptionTextKv, 3> j2k_options{{
    {"target_psnr", "45"},
    {"threads", "4"},
    {"color_transform", "true"},
}};

// 在重复写入循环之外构建一个可重用的 JPEG 2000 上下文。
// 这将传输语法和选项集保留在一处，而不是
// 在每个调用站点重建相同的选项列表。
auto j2k_ctx = dicom::pixel::create_encoder_context(
    "JPEG2000"_uid,
    std::span<const dicom::pixel::CodecOptionTextKv>(j2k_options));

try {
    for (const char* path : {"out_j2k_1.dcm", "out_j2k_2.dcm"}) {
        file->write_with_transfer_syntax(path, "JPEG2000"_uid, j2k_ctx);
    }
} catch (const dicom::diag::DicomException& ex) {
    // 当encode或configure失败时，异常消息中携带
    // 失败呼叫的阶段/原因详细信息。记录 ex.what() 通常是
    // 足以进行第一次调试。
    std::cerr << ex.what() << '\n';
}
```

### 直接写一个不同的传输语法来输出

```cpp
#include <dicom.h>

using namespace dicom::literals;

auto file = dicom::read_file("sample.dcm");

// write_with_transfer_syntax() 是面向输出的转码路径，当
// 目标语法仅对序列化结果重要。
file->write_with_transfer_syntax("out_rle.dcm", "RLELossless"_uid);

// 同一 API 系列还具有 C++ 中的 std::ostream 重载。
```

### 传递显式编解码器选项

```cpp
#include <array>
#include <dicom.h>

using namespace dicom::literals;

auto file = dicom::read_file("sample.dcm");

const std::array<dicom::pixel::CodecOptionTextKv, 1> lossy_options{{
    {"target_psnr", "45"},
}};

// 对于有损目标，请明确传递您想要的编解码器选项，而不是
// 依赖的默认值可能与您的预期输出不匹配。这个直接
// 风格适合一次性写入；相同时使用 EncoderContext 代替
// 选项集将在多次调用中重复使用。
file->write_with_transfer_syntax(
    "out_j2k_lossy.dcm", "JPEG2000"_uid,
    std::span<const dicom::pixel::CodecOptionTextKv>(lossy_options));
```

## Python

### 替换 NumPy 数组中的像素数据

```python
import dicomsdl as dicom
import numpy as np

df = dicom.read_file("sample.dcm")

# set_pixel_data() 需要一个 C 连续数值数组。
# 数组形状和数据类型决定了编码的 Rows、Columns、
# SamplesPerPixel、NumberOfFrames 和位深度元数据。
rng = np.random.default_rng(0)
arr = rng.integers(0, 4096, size=(256, 256), dtype=np.uint16)
df.set_pixel_data("ExplicitVRLittleEndian", arr)

df.write_file("native_replaced.dcm")
```

### 将显式编解码器选项传递给 `set_pixel_data()`

```python
import dicomsdl as dicom
import numpy as np

df = dicom.read_file("sample.dcm")
rng = np.random.default_rng(0)
arr = rng.integers(0, 4096, size=(256, 256), dtype=np.uint16)

# 对于有损目标，显式传递编解码器选项，以便编码设置
# 在调用站点可见。
df.set_pixel_data(
    "JPEG2000",
    arr,
    options={"type": "j2k", "target_psnr": 45.0},
)
```

### 解析并验证 Python 选项字典一次，然后重用上下文

```python
import dicomsdl as dicom

df = dicom.read_file("sample.dcm")

# create_encoder_context() 在这里解析并验证 Python 选项对象，
# 一次，在重复写入循环开始之前。
j2k_ctx = dicom.create_encoder_context(
    "JPEG2000",
    options={
        "type": "j2k",
        "target_psnr": 45.0,
        "threads": 4,
        "color_transform": True,
    },
)

# 对重复输出重复使用相同的经过验证的传输语法和选项集。
for path in ("out_j2k_1.dcm", "out_j2k_2.dcm"):
    df.write_with_transfer_syntax(path, "JPEG2000", encoder_context=j2k_ctx)
```

### 在编码循环开始之前检查配置错误

```python
import dicomsdl as dicom

try:
    dicom.create_encoder_context(
        "JPEG2000",
        options={
            "type": "j2k",
            "target_psnr": -1.0,
        },
    )
except ValueError as exc:
    # 在任何长时间运行的编码循环开始之前，无效选项在这里失败。
    print(exc)
```

### 编写不同的传输语法而不改变源对象

```python
import dicomsdl as dicom

df = dicom.read_file("sample.dcm")

# write_with_transfer_syntax() 仅更改序列化输出路径。
# 内存中的 DicomFile 保留其当前的传输语法和像素状态。
df.write_with_transfer_syntax("out_rle.dcm", "RLELossless", options="rle")
```

## 例外情况

**C++**

|应用程序接口 |抛出异常 |典型原因 |
| --- | --- | --- |
| `create_encoder_context(...)` / `EncoderContext::configure(...)` | `dicom::diag::DicomException` |传输语法无效或不支持编码。在 C++ 中，当编码或转码调用配置运行时编码器时，大多数编解码器选项语义仍会在稍后进行验证。 |
| `set_pixel_data(...)` | `dicom::diag::DicomException` |源布局和源字节不一致、编码器上下文丢失或不匹配、编码器绑定不可用、后端拒绝当前编解码器选项或像素布局，或者编码后传输语法元数据更新失败。 |
| `set_transfer_syntax(...)` | `dicom::diag::DicomException` |传输语法选择无效、编码器上下文与请求的语法不匹配、转码路径不受支持或后端编码失败。 |
| `write_with_transfer_syntax(...)` | `dicom::diag::DicomException` |传输语法选择无效、编码器上下文与请求的语法不匹配、转码路径不受支持、后端编码失败或文件/流输出失败。 |

C++编码消息通常包括`status=...`、`stage=...`、`reason=...`，状态为`invalid_argument`、`unsupported`、`backend_error`、`internal_error`等。

**Python**

|应用程序接口 |Python 异常|典型原因 |
| --- | --- | --- |
| `create_encoder_context(...)` | `TypeError`、`ValueError`、`RuntimeError` | `options` 的容器或值类型错误、选项键或值无效、传输语法文本未知或底层 C++ 配置步骤仍然失败。 |
| `set_pixel_data(...)` | `TypeError`、`ValueError`、`RuntimeError` | `source` 不是受支持的缓冲区对象，不是 C 连续的，推断的源形状或数据类型无效，编码选项无效，或者运行时编码器/数据集更新失败。 |
| `set_transfer_syntax(...)` | `TypeError`、`ValueError`、`RuntimeError` |传输语法文本无效、`options` 对象类型错误、选项值无效、编码器上下文与请求的语法不匹配或转码路径/后端失败。 |
| `write_with_transfer_syntax(...)` | `TypeError`、`ValueError`、`RuntimeError` |路径或 `options` 类型无效、传输语法文本或选项值无效、编码器上下文与请求的语法不匹配或写入/转码失败。 |

## 注释

- 在 C++ 中，`set_pixel_data()` 从您提供的 `pixel::ConstPixelSpan` 布局中读取本机像素。如果源字节具有行间距或帧间距，则布局必须准确描述该间距。
- 在Python中，`set_pixel_data()`需要一个C连续的数字缓冲区。如果数组当前是跨步的或不连续的，请首先使用 `np.ascontiguousarray(...)`。
- `set_pixel_data()` 重写相关图像像素元数据，例如 `Rows`、`Columns`、`SamplesPerPixel`、`BitsAllocated`、`BitsStored`、`PhotometricInterpretation`、`NumberOfFrames` 和传输语法状态。
- `set_transfer_syntax()` 改变内存中的 `DicomFile`。当目标只是不同编码的输出文件或流时，`write_with_transfer_syntax()` 是更好的路径。
- 当重复应用相同的传输语法和编解码器选项时，重复使用 `EncoderContext`。在Python中，`create_encoder_context(..., options=...)`还预先解析和验证`options`对象。在 C++ 中，`EncoderContext` 将一种传输语法和选项集保留在一起，而详细的故障仍然表现为 `dicom::diag::DicomException`。
- 对于确切的编解码器规则、选项名称和每个传输语法约束，请使用参考页，而不是从简短的示例中猜测。

## 相关文档

- [像素解码](pixel_decode.md)
- [文件I/O](file_io.md)
- [像素编码约束](../reference/pixel_encode_constraints.md)
