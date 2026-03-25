# CLI 工具

DicomSDL 提供 3 个面向用户的命令行工具。

- `dicomdump`：输出便于阅读的 DICOM dump
- `dicomshow`：通过 Pillow 快速预览单帧图像
- `dicomconv`：修改 transfer syntax 并写出新文件

## 如何安装这些命令

### 通过 Python wheel 安装

```bash
pip install dicomsdl
```

这会安装 `dicomdump`、`dicomconv`、`dicomshow` 三个控制台脚本。

`dicomshow` 走的是 Pillow 预览路径，所以实际使用中通常更适合安装：

```bash
pip install "dicomsdl[numpy,pil]"
```

### 从源码构建后使用

如果用 `-DDICOM_BUILD_EXAMPLES=ON` 构建 C++ 示例，build 目录中会生成
`dicomdump` 和 `dicomconv` 可执行文件：

```bash
cmake -S . -B build -DDICOM_BUILD_EXAMPLES=ON
cmake --build build

./build/dicomdump sample.dcm
./build/dicomconv in.dcm out.dcm ExplicitVRLittleEndian
```

没有单独的 C++ 版 `dicomshow` 可执行文件。`dicomshow` 是 Python 控制台脚本 entry point。

## `dicomdump`

当你想把一个或多个 DICOM 文件以可读文本形式查看时，用 `dicomdump`。
它会读取每个文件，并输出 `DicomFile.dump(...)` 的结果。

### `dicomdump` 用法

```bash
dicomdump [--max-print-chars N] [--no-offset] [--with-filename] <file> [file...]
```

### `dicomdump` 参考

位置参数：

| 参数 | 含义 |
| --- | --- |
| `paths` | 一个或多个输入路径。CLI 也会展开 `*.dcm` 这样的 wildcard 模式。 |

选项：

| 选项 | 含义 |
| --- | --- |
| `--max-print-chars N` | 长 printable value 在 `N` 个字符后截断。默认值是 `80`。 |
| `--no-offset` | 隐藏 `OFFSET` 列。 |
| `--with-filename` | 在每一行输出前加上 `filename:`。输入多个文件时默认会启用。 |

### `dicomdump` 示例

```bash
dicomdump sample.dcm
dicomdump a.dcm b.dcm
dicomdump --no-offset --max-print-chars 120 sample.dcm
dicomdump "*.dcm"
```

当输入多个文件时，`dicomdump` 会在每一行前加上源文件名，避免输出混在一起后难以分辨。

## `dicomshow`

`dicomshow` 适合在 shell 中做快速目视检查。它会读取一个 DICOM 文件，用
`to_pil_image(frame=...)` 生成单帧图像，然后调用 `Pillow.Image.show()`。

### `dicomshow` 用法

```bash
dicomshow [--frame N] <input.dcm>
```

### `dicomshow` 参考

位置参数：

| 参数 | 含义 |
| --- | --- |
| `input` | 输入 DICOM 文件路径。 |

选项：

| 选项 | 含义 |
| --- | --- |
| `--frame N` | 预览的 0-based 帧索引。默认是 `0`。 |

### `dicomshow` 说明

- `dicomshow` 是快速预览工具，不是诊断级 viewer。
- 它依赖本地 GUI / viewer 关联，在 headless 环境中可能无法工作。
- 如果没有 Pillow 或 NumPy，请安装 `DicomSDL[numpy,pil]`。

### `dicomshow` 示例

```bash
dicomshow sample.dcm
dicomshow --frame 5 multiframe.dcm
```

## `dicomconv`

如果你想在 shell 脚本或终端中做文件到文件的 transfer syntax 转换，就用 `dicomconv`。

它的内部流程是：读入输入文件，调用 `set_transfer_syntax(...)`，再把结果写到新的路径。

### `dicomconv` 用法

```bash
dicomconv <input.dcm> <output.dcm> <transfer-syntax> [options]
```

`<transfer-syntax>` 可以是：

- `ExplicitVRLittleEndian` 这样的 transfer syntax keyword
- `1.2.840.10008.1.2` 这样的 dotted UID 字符串
- `jpeg`、`jpeg2k`、`htj2k-lossless`、`jpegxl` 这样的 shortcut alias

### `dicomconv` 参考

位置参数：

| 参数 | 含义 |
| --- | --- |
| `input` | 输入 DICOM 文件路径。 |
| `output` | 输出 DICOM 文件路径。 |
| `transfer_syntax` | 目标 transfer syntax keyword、dotted UID 字符串，或 shortcut alias。 |

选项：

| 选项 | 适用范围 | 含义 |
| --- | --- | --- |
| `--codec {auto,none,rle,jpeg,jpegls,j2k,htj2k,jpegxl}` | 全部 | 不只依赖目标 transfer syntax 推断，而是显式指定 codec 选项类型。 |
| `--quality N` | `jpeg` | JPEG quality，范围 `[1, 100]`。 |
| `--near-lossless-error N` | `jpegls` | JPEG-LS `NEAR`，范围 `[0, 255]`。 |
| `--target-psnr V` | `j2k`, `htj2k` | target PSNR。 |
| `--target-bpp V` | `j2k`, `htj2k` | target bits-per-pixel。 |
| `--threads N` | `j2k`, `htj2k`, `jpegxl` | encoder thread 设置。`-1` 表示 auto，`0` 表示库默认值。 |
| `--color-transform` | `j2k`, `htj2k` | 开启 MCT color transform。 |
| `--no-color-transform` | `j2k`, `htj2k` | 关闭 MCT color transform。 |
| `--distance V` | `jpegxl` | JPEG-XL distance，范围 `[0, 25]`。`0` 表示 lossless。 |
| `--effort N` | `jpegxl` | JPEG-XL effort，范围 `[1, 10]`。 |

完整 help、示例以及当前支持的 target transfer syntax 列表，请运行 `dicomconv -h`。

### `dicomconv` 示例

```bash
dicomconv in.dcm out.dcm ExplicitVRLittleEndian
dicomconv in.dcm out.dcm 1.2.840.10008.1.2
dicomconv in.dcm out.dcm jpeg --quality 92
dicomconv in.dcm out.dcm jpegls-near-lossless --near-lossless-error 3
dicomconv in.dcm out.dcm jpeg2k --target-psnr 45 --threads -1
dicomconv in.dcm out.dcm htj2k-lossless --no-color-transform
dicomconv in.dcm out.dcm jpegxl --distance 1.5 --effort 7 --threads -1
```

## 退出码

这三个命令都遵循同样的规则：

- 成功时返回 `0`
- 输入、parse、decode、encode、write 任一步失败时返回 `1`

错误会带着 `dicomdump:`、`dicomshow:`、`dicomconv:` 这样的前缀输出到标准错误。

## 相关文档

- [Installation](installation.md)
- [File I/O](file_io.md)
- [Pixel Decode](pixel_decode.md)
- [Pixel Encode](pixel_encode.md)
- [Pixel Encode Constraints](../reference/pixel_encode_constraints.md)
