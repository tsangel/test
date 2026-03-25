# 像素解码

当您需要在解码前获得经过验证的解码布局、需要分配或重用自己的输出缓冲区、需要显式指定解码后的行或帧步长，或者希望单帧和多帧输入共用同一套代码路径时，请将 `create_decode_plan()` 与 `decode_into()` 一起使用。在 C++ 中使用 `pixel_buffer()`，在 Python 中使用 `to_array()`，即可走最简单、直接返回新解码结果的路径。

## 关键解码 API

**C++**

- `create_decode_plan(...)` + `decode_into(...)`
- 当您需要经过验证、可重复使用的解码布局，并且要使用调用方自己提供的输出缓冲区时，请使用这组 API。这也包括单帧场景下提前分配或复用缓冲区、或通过 `DecodeOptions` 明确指定输出步长的情况。
- `pixel_buffer(...)`
- 解码并返回新的像素缓冲区。

**Python**

- `create_decode_plan(...)` + `decode_into(...)`
- 当您需要经过验证、可重复使用的解码布局，并且要使用调用方提供的可写数组或缓冲区时，请使用这组 API。这也包括单帧场景下提前准备目标缓冲区，或通过 `DecodeOptions` 明确指定输出步幅的情况。
- `to_array(...)`
- 解码并返回一个新的 NumPy 数组。这是最快、也最容易直接拿到结果的路径。
- `to_array_view(...)`
- 当源像素数据使用未压缩的传输语法时，返回零拷贝 NumPy 视图。

## 相关 DICOM 标准部分

- 控制行、列、每像素样本、光度解释和像素数据的像素属性位于 [DICOM PS3.3 第 C.7.6.3 节，图像像素模块](https://dicom.nema.org/medical/dicom/current/output/chtml/part03/sect_C.7.6.3.html)。
- 原生与封装像素数据编码在 [DICOM PS3.5 第 8 章，像素、叠加和波形数据编码](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/chapter_8.html) 和 [第 8.2 节，原生或封装格式编码](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/sect_8.2.html)。
- 封装的片段/项目布局和传输语法要求在 [DICOM PS3.5 第 A.4 节，编码像素数据封装的传输语法](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/sect_A.4.html) 中定义。
- 在基于文件的工作流程中，传输语法 UID 来自 [DICOM PS3.10 第 7 章，DICOM 文件格式](https://dicom.nema.org/medical/dicom/current/output/chtml/part10/chapter_7.html) 中描述的文件元信息。

## C++

### 在解码一帧之前检查输出布局

```cpp
#include <cstdint>
#include <dicom.h>
#include <span>
#include <vector>

auto file = dicom::read_file("single_frame.dcm");

// 该计划不保存解码的像素。
// 相反，它验证当前文件元数据并告诉我们什么
// 解码的输出必须与我们分配任何目标内存之前的样子相同。
const auto plan = file->create_decode_plan();

// 对于单帧解码，frame_stride 是准确的字节数
// decode_into() 期望使用此计划获得一个解码帧。
std::vector<std::uint8_t> out(plan.output_layout.frame_stride);

// 帧 0 是这里唯一的帧，但此调用形状也适用于
// 多帧输入。这使得保留一个调用者拥有的缓冲区路径变得容易
// 对于单帧和多帧代码。
file->decode_into(0, std::span<std::uint8_t>(out), plan);

// `out` 现在包含一个完全按照计划描述布置的解码帧。
```

### 在多个帧中重复使用一个计划和一个目标缓冲区

```cpp
#include <cstdint>
#include <dicom.h>
#include <span>
#include <vector>

auto file = dicom::read_file("multiframe.dcm");
const auto plan = file->create_decode_plan();

// 一个 DecodePlan 意味着一个解码帧布局，因此我们可以分配一个
// 可重用的帧缓冲区并为每个帧重新填充它。
std::vector<std::uint8_t> frame_bytes(plan.output_layout.frame_stride);

for (std::size_t frame = 0; frame < plan.output_layout.frames; ++frame) {
	// 对每个帧重复使用相同的经过验证的布局，而不是重新计算
	// 元数据或每次分配一个新的缓冲区。
	file->decode_into(frame, std::span<std::uint8_t>(frame_bytes), plan);

	// 在下一次迭代之前在此处处理、复制或转发 `frame_bytes`
	// 用下一个解码帧覆盖它。
}
```

### 从 DecodeOptions 制定计划

```cpp
#include <cstdint>
#include <dicom.h>
#include <span>
#include <vector>

auto file = dicom::read_file("multiframe_j2k.dcm");

dicom::pixel::DecodeOptions options{};
options.alignment = 32;
// 当解码图像每个像素有多个样本时，要求平面输出。
options.planar_out = dicom::pixel::Planar::planar;
// 当后端应用码流级逆MCT/颜色变换
// 支持它。这是默认的、通常的起点。
options.decode_mct = true;
// 外部工作线程调度主要针对批量或多工作项解码。
options.worker_threads = 4;
// 要求编解码器后端在支持时使用最多两个内部线程。
options.codec_threads = 2;

// 该计划捕获了这些选项以及它们所暗示的确切输出布局。
const auto plan = file->create_decode_plan(options);

// 对于全卷解码，请为每个解码帧分配足够的存储空间。
std::vector<std::uint8_t> volume(
    plan.output_layout.frames * plan.output_layout.frame_stride);

// decode_all_frames_into() 使用相同的经过验证的计划，但填充了整个
// 输出音量而不是一次一帧。
file->decode_all_frames_into(std::span<std::uint8_t>(volume), plan);
```

### 请求显式输出步幅

```cpp
#include <cstdint>
#include <dicom.h>
#include <span>
#include <vector>

auto file = dicom::read_file("multiframe.dcm");

const auto rows = static_cast<std::size_t>(file["Rows"_tag].to_long().value_or(0));
const auto cols = static_cast<std::size_t>(file["Columns"_tag].to_long().value_or(0));
const auto samples_per_pixel =
    static_cast<std::size_t>(file["SamplesPerPixel"_tag].to_long().value_or(1));
const auto frame_count =
    static_cast<std::size_t>(file["NumberOfFrames"_tag].to_long().value_or(1));
const auto bits_allocated =
    static_cast<std::size_t>(file["BitsAllocated"_tag].to_long().value_or(0));
const auto bytes_per_sample = (bits_allocated + 7) / 8;
const auto packed_row_bytes = cols * samples_per_pixel * bytes_per_sample;
const auto row_stride = ((packed_row_bytes + 32 + 31) / 32) * 32;
const auto frame_stride = row_stride * rows;

// 首先从元数据派生的布局分配目标缓冲区。
std::vector<std::uint8_t> frame_bytes(frame_stride);
std::vector<std::uint8_t> volume_bytes(frame_count * frame_stride);

dicom::pixel::DecodeOptions options{};
// 交错输出是默认值，但在这里将其拼写出来，因为步幅
// 下面的计算假设每行内都有交错样本。
options.planar_out = dicom::pixel::Planar::interleaved;
// 在打包行有效负载之外添加至少 32 个字节，然后向上舍入到
// 下一个 32 字节边界。
options.row_stride = row_stride;
options.frame_stride = frame_stride;

const auto plan = file->create_decode_plan(options);

// 该计划验证了我们上面选择的显式行/帧步幅。
file->decode_into(0, std::span<std::uint8_t>(frame_bytes), plan);
file->decode_all_frames_into(std::span<std::uint8_t>(volume_bytes), plan);
```

## Python

### 在解码一帧之前检查输出布局

```python
import dicomsdl as dicom
import numpy as np

df = dicom.read_file("single_frame.dcm")

# 这个 plan 会在真正开始解码之前给出解码结果的 dtype 和数组 shape。
# 当调用方想先分配目标数组时，这一点很有用。
plan = df.create_decode_plan()

# 先从 plan 获取 frame 0 的准确 NumPy 数组 shape。
# 这样就能让这次分配与后面的 decode_into() 调用保持同一套布局约定。
out = np.empty(plan.shape(frame=0), dtype=plan.dtype)

# 重用已经验证的计划，而不是在此处重新计算布局元数据。
df.decode_into(out, frame=0, plan=plan)

# 现在 `out` 中包含一个按 plan 指定布局解码出的 frame。
```

### 在多个帧中重复使用一个计划和一个目标数组

```python
import dicomsdl as dicom
import numpy as np

df = dicom.read_file("multiframe.dcm")
plan = df.create_decode_plan()

# 对于一个计划，每一帧都具有相同的解码形状，因此一个可重用的数组是
# 对于逐帧处理循环来说足够了。
frame_out = np.empty(plan.shape(frame=0), dtype=plan.dtype)

for frame in range(plan.frames):
    # decode_into() 每次都会覆盖相同的目的地，同时重用
    # 相同的经过验证的计划。
    df.decode_into(frame_out, frame=frame, plan=plan)

    # 在下一次迭代之前在此处处理、复制或转发 `frame_out`
    # 将其重新用于下一个解码帧。
```

### 从 DecodeOptions 制定计划

```python
import dicomsdl as dicom
import numpy as np

df = dicom.read_file("multiframe_j2k.dcm")

options = dicom.DecodeOptions(
    alignment=32,
    planar_out=dicom.Planar.planar,
    # 当后端应用码流级逆MCT/颜色变换
    # 支持它。这是默认的、通常的起点。
    decode_mct=True,
    # 外部工作者调度主要针对批量或多帧解码。
    worker_threads=4,
    # 要求编解码器后端在支持时使用最多两个内部线程。
    codec_threads=2,
)

# 该计划捕获请求的解码行为，因此以后的解码调用可以
# 只需重复使用 `plan` 即可，无需重复选项。
plan = df.create_decode_plan(options)

# frame=-1 表示“所有帧”。该计划可以告诉我们确切的全体积形状
# 在我们分配目标数组之前。
volume = np.empty(plan.shape(frame=-1), dtype=plan.dtype)

# 当提供 plan=... 时，计划的捕获选项将驱动解码。
df.decode_into(volume, frame=-1, plan=plan)
```

### 请求显式输出步幅

```python
import dicomsdl as dicom
import numpy as np

df = dicom.read_file("test_le.dcm")

options = dicom.DecodeOptions(
    # 交错输出是默认值，但在这里将其拼写出来，因为这
    # 示例描述了交错布局中的行步幅。
    planar_out=dicom.Planar.interleaved,
    # 对于这个小示例文件，使用较大的行跨距来进行自定义
    # 布局明显。对于您自己的文件，选择一个大于打包的值
    # 解码的行大小。
    row_stride=1024,
)
plan = df.create_decode_plan(options)

# to_array(plan=...) 返回一个数组，其 NumPy 步长与计划匹配。
# 这意味着当计划使用时结果可能故意不连续
# 明确的行或帧步幅。
arr = df.to_array(frame=0, plan=plan)

# `arr.strides` 现在反映了请求的输出步幅，因此数组
# 即使解码的像素值是正确的，也可能故意不连续。
```

### 解码为原始存储以实现自定义步长 NumPy 视图

```python
import dicomsdl as dicom
import numpy as np

df = dicom.read_file("test_le.dcm")
plan = df.create_decode_plan(
    # 这个小样本故意使用了超大的行跨距，因此自定义
    # NumPy视图很容易看到。对于您自己的文件，选择一个较大的值
    # 足够解码一行。
    dicom.DecodeOptions(row_stride=1024)
)

# decode_into() 仍然需要一个可写的 C 连续输出缓冲区对象。
# 对于自定义步幅布局，分配一个原始的一维缓冲区，其数量恰好为
# 计划所需的解码字节数。
raw = np.empty(
    plan.required_bytes(frame=0) // plan.bytes_per_sample,
    dtype=plan.dtype,
)
df.decode_into(raw, frame=0, plan=plan)

# 将原始存储包装在 NumPy 视图中，其显式步长与计划匹配。
# 这个单帧单色示例变成了自定义跨度二维数组视图，无需
# 额外的像素副本。
arr = np.ndarray(
    shape=plan.shape(frame=0),
    dtype=plan.dtype,
    buffer=raw,
    strides=(plan.row_stride, plan.bytes_per_sample),
)
```

### 首先准备NumPy存储，然后将多帧输出解码到其中

```python
import dicomsdl as dicom
import numpy as np

df = dicom.read_file("multiframe.dcm")

# 此示例假定单色 uint16 多帧输出布局。
# 当解码的数据类型或示例布局不同时，请先调整这些值。
dtype = np.uint16
itemsize = np.dtype(dtype).itemsize
rows = int(df.Rows)
cols = int(df.Columns)
frame_count = int(df.NumberOfFrames)
packed_row_bytes = cols * itemsize
# 在打包行有效负载之外添加至少 32 个字节，然后向上舍入到
# 下一个 32 字节边界。
row_stride = ((packed_row_bytes + 32 + 31) // 32) * 32
frame_stride = row_stride * rows

# 首先将后备存储准备为普通的一维 C 连续 NumPy 数组。
# 这是 decode_into() 将写入的对象。
backing = np.empty((frame_stride * frame_count) // itemsize, dtype=dtype)

# 解码之前在同一存储上构建面向应用程序的数组视图。
# 此示例使用以帧为主的单色布局：
#   （帧、行、列），步幅为（frame_stride、row_stride、项目大小）。
frames = np.ndarray(
    shape=(frame_count, rows, cols),
    dtype=dtype,
    buffer=backing,
    strides=(frame_stride, row_stride, itemsize),
)

# 仓储布局确定后，制定配套计划。
plan = df.create_decode_plan(
    dicom.DecodeOptions(
        # 交错输出是默认值，但在这里将其拼写出来，因为
        # 上面的存储布局是为交错的样品准备的。
        planar_out=dicom.Planar.interleaved,
        row_stride=row_stride,
        frame_stride=frame_stride,
    )
)

# 确认该计划与我们手动准备的 NumPy 布局相匹配。
assert plan.dtype == np.dtype(dtype)
assert plan.bytes_per_sample == itemsize
assert plan.shape(frame=-1) == frames.shape
assert plan.row_stride == row_stride
assert plan.frame_stride == frame_stride
assert plan.required_bytes(frame=-1) == backing.nbytes

# decode_into() 仍然要求目标对象本身可写并且
# C-连续。这就是我们在这里传递 `backing` 的原因。
df.decode_into(backing, frame=-1, plan=plan)

# `frames` 现在通过您准备的 NumPy 布局公开解码的像素
# 提前，而 `backing` 继续拥有底层存储。
```

### 显式处理 C++ 解码失败

```cpp
#include <dicom.h>
#include <diagnostics.h>
#include <iostream>
#include <span>
#include <vector>

try {
    auto file = dicom::read_file("single_frame.dcm");
    const auto plan = file->create_decode_plan();

    std::vector<std::uint8_t> out(plan.output_layout.frame_stride);
    file->decode_into(0, std::span<std::uint8_t>(out), plan);
} catch (const dicom::diag::DicomException& ex) {
    // 该消息通常包括 status=...、stage=... 和 Reason=...
    // 因此，一条日志行通常足以查看故障是否来自
    // 元数据验证、目的地验证、解码器选择或
    // 后端解码步骤本身。
    std::cerr << ex.what() << '\n';
}
```

### 显式处理 Python 解码失败

```python
import dicomsdl as dicom
import numpy as np

df = dicom.read_file("single_frame.dcm")

try:
    plan = df.create_decode_plan()
    out = np.empty(plan.shape(frame=0), dtype=plan.dtype)
    df.decode_into(out, frame=0, plan=plan)
except (TypeError, ValueError, IndexError) as exc:
    # 绑定端验证失败发生在此处：
    # 错误的缓冲区类型、错误的输出大小、无效的帧索引等。
    print(exc)
except RuntimeError as exc:
    # RuntimeError 通常表示底层 C++ 解码路径在之后失败
    # Python 参数被接受。
    print(exc)
```

## 例外情况

**C++**

|应用程序接口 |抛出异常 |典型原因 |
| --- | --- | --- |
| `create_decode_plan(...)` | `dicom::diag::DicomException` |像素元数据丢失或不一致，`alignment` 无效，显式 `row_stride` / `frame_stride` 小于解码的有效负载，或者输出布局溢出。 |
| `decode_into(...)` | `dicom::diag::DicomException` |计划不再与当前文件状态匹配、帧索引超出范围、目标缓冲区太小、解码器绑定不可用或后端解码失败。 |
| `pixel_buffer(...)` | `dicom::diag::DicomException` |与 `decode_into(...)` 相同的故障模式，但位于拥有缓冲区便利路径上。 |
| `decode_all_frames_into(...)` | `dicom::diag::DicomException` |全卷目标太小、帧元数据无效、解码器绑定不可用、后端解码失败或 `ExecutionObserver` 取消批次。 |

C++解码消息通常包括`status=...`、`stage=...`、`reason=...`，状态为`invalid_argument`、`unsupported`、`backend_error`、`cancelled`、`internal_error`等。

**Python**

|应用程序接口 |Python 异常|典型原因 |
| --- | --- | --- |
| `create_decode_plan(...)` | `RuntimeError` |由于像素元数据丢失、请求的输出布局无效或解码的布局溢出，底层 C++ 计划创建失败。 |
| `to_array(...)` | `ValueError`、`IndexError`、`RuntimeError` | `frame < -1`、无效线程计数、帧索引超出范围或参数验证成功后底层解码失败。 |
| `decode_into(...)` | `TypeError`、`ValueError`、`IndexError`、`RuntimeError` |目标不是可写的 C 连续缓冲区、项目大小或总字节大小与解码的布局不匹配、帧索引超出范围或底层解码路径失败。 |
| `to_array_view(...)` | `ValueError`、`IndexError` |源传输语法已压缩，多样本本机数据未交错，没有直接原始像素视图可用，或者帧索引超出范围。 |

## 注释

- 即使对于单帧输入，当您想要在解码之前检查输出布局或跨调用重用目标缓冲区时，`DecodePlan` 也很有用。
- 将 `DecodePlan` 视为经过验证的输出合约，而不是解码像素的缓存。
- `DecodeOptions.row_stride` 和 `DecodeOptions.frame_stride` 允许您请求显式行和帧步长以进行解码输出。当任一非零时，`alignment` 被忽略。
- 显式解码步幅对于解码行或帧有效负载仍然必须足够大，并与解码样本大小对齐。
- 如果您改变影响像素的元数据，例如传输语法、行、列、每个像素的样本、分配的位、像素表示、平面配置、帧数或像素数据元素，请勿重复使用旧的解码布局假设。
- 如果影响像素的元数据发生更改，请在下一个 `decode_into()` 之前创建一个新的 `DecodePlan` 和匹配的输出缓冲区。
- `decode_into()` 是基准测试或热循环重用场景的正确路径，或者当您希望单帧和多帧输入具有相同的缓冲区管理流程时。
- 在 Python 中，当计划请求显式行或帧步幅时，`to_array(plan=...)` 可能会返回具有自定义步幅的 NumPy 数组，而不是打包的 C 连续数组。
- 在 Python 中，`decode_into()` 需要可写的 C 连续目标对象。对于自定义步幅结果，解码为连续的后备存储，然后通过具有显式步幅的 NumPy 视图公开它。
- `to_array()`是最快首次成功的正确途径。

## 相关文档

- [快速入门](quickstart.md)
- [像素编码](pixel_encode.md)
- [像素变换元数据解析](../reference/pixel_transform_metadata.md)
