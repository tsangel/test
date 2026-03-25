# 安装

如果你需要可重复的安装 / 构建路径，或者希望更清楚地了解 DicomSDL 当前面向哪些环境，请使用本页。

## 环境摘要

### Python / PyPI

- CPython `3.9` to `3.14`
- 当前 wheel CI 目标：
  - Linux `x86_64`
  - macOS `x86_64`
  - macOS `arm64`
  - Windows `AMD64`
- 当前不构建 `musllinux` wheel
- base install 足以支持 metadata access、file I/O 和 transcode workflow
- 为 NumPy 支持的 pixel helper 和 Pillow 预览提供 optional extra

### C++ / source build

- `git`
- `CMake 3.16+`
- `C++20` compiler
- 推荐使用 `Ninja`，但只有某些 Windows toolchain 才要求它
- 只有在构建 Python binding 或 wheel 时才需要 Python

### Windows toolchain

`build.bat` 文档化并支持以下 source-build 路径：

- `MSVC` (`cl.exe`)
- `clang-cl` with `Ninja`
- `MSYS2 clang64` with `Ninja`

如果没有设置 `DICOMSDL_WINDOWS_TOOLCHAIN`，`build.bat` 会根据 `PATH` 中可用的工具按 `msvc`、`clangcl`、`clang64` 的顺序自动选择。

### macOS deployment target

- `x86_64` builds default to macOS `10.15`
- `arm64` builds default to macOS `11.0`
- 只有在需要显式控制时才设置 `MACOSX_DEPLOYMENT_TARGET`

## 选择路径

- PyPI install: 体验 Python binding 的最快路径
- 从 checkout 构建 C++: 当你要把 DicomSDL 集成到自己的 C++ application 时最合适
- Unix-like `build.sh`: macOS / Linux 构建的 convenience wrapper
- 从 checkout 构建 Python source: 记录在 [Build Python From Source](../developer/build_python_from_source.md)
- Windows `build.bat`: 当你需要显式选择 toolchain 时的推荐入口

## PyPI 安装

```bash
python -m pip install --upgrade pip
pip install dicomsdl
```

如果当前平台上 `pip` 回退到 source build，请先安装 `cmake`。

可选 Python extra：

- `pip install "dicomsdl[numpy]"`: 用于 `to_array(...)`、`decode_into(...)` 和 NumPy 支持的 pixel helper
- `pip install "dicomsdl[numpy,pil]"`: 如果还需要 `to_pil_image(...)` convenience helper (`numpy` + `Pillow`)

如果你在服务器侧只需要 metadata access、file I/O 或 transcode workflow，基础的 `pip install dicomsdl` 就足够了。

## 从 checkout 构建 C++

```bash
git clone https://github.com/tsangel/dicomsdl.git
cd dicomsdl
cmake -S . -B build -DDICOM_BUILD_EXAMPLES=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

在 Linux 和 macOS 上，这是标准的 CMake 路径。在 Windows 上，如果你还没有固定到特定 generator / toolchain，更推荐使用 `build.bat`。

## Unix-like `build.sh`

在 macOS 和 Linux 上，wrapper script 如下：

```bash
./build.sh
```

如果只想构建指定 target：

```bash
./build.sh dicomsdl
./build.sh dicomsdl _dicomsdl
```

尾部参数会作为 `cmake --build --target ...` 的 target 传递。

## Windows `build.bat`

最短的 Windows 入口如下：

```bat
call build.bat dicomsdl
```

如果想强制使用特定 toolchain：

```bat
set DICOMSDL_WINDOWS_TOOLCHAIN=msvc
call build.bat dicomsdl
```

需要时可将 `msvc` 换成 `clangcl` 或 `clang64`。

`MSVC` 和 `clang-cl` 请在已初始化的 Visual Studio developer environment 中使用。`clang64` 则应在 MSYS2 `clang64` 环境中运行，并确保 `clang`、`clang++`、`cmake`、`ninja` 在 `PATH` 中可用。

与 `build.sh` 一样，尾部参数会作为 `cmake --build --target ...` 的 target 传递。

## 通用 build script 选项

`build.sh` 和 `build.bat` 使用相同的核心环境变量：

- `BUILD_DIR`: build directory 路径
- `BUILD_TYPE`: 如 `Release`、`Debug` 之类的 build configuration
- `BUILD_TESTING=ON|OFF`: 启用或禁用 CTest target
- `DICOM_BUILD_EXAMPLES=ON|OFF`: 是否构建 example binary
- `RUN_TESTS=1|0`: 是否在构建后运行 CTest
- `BUILD_WHEEL=1|0`: 是否执行 Python wheel 步骤
- `WHEEL_ONLY=1`: 跳过 top-level CMake configure/build，只构建 wheel
- `WHEEL_DIR=...`: wheel 输出目录
- `PYTHON_BIN=...`: wheel 构建使用的 Python interpreter
- `CMAKE_EXTRA_ARGS="..."`: 追加原始 CMake configure 参数
- `BUILD_PARALLELISM=N`: 覆盖自动检测到的并行构建数
- `RESET_CMAKE_CACHE=1`: 当 generator/compiler 设置变化时清理旧的 CMake cache
- `DICOMSDL_PIXEL_DEFAULT_MODE=builtin|shared|none`: 默认 codec plugin mode
- `DICOMSDL_PIXEL_<CODEC>_MODE=builtin|shared|none`: 针对 `JPEG`、`JPEGLS`、`JPEG2K`、`HTJ2K`、`JPEGXL` 的单独 override

示例：

```bash
BUILD_TESTING=OFF BUILD_WHEEL=0 ./build.sh dicomsdl
BUILD_PARALLELISM=8 CMAKE_EXTRA_ARGS="-DDICOM_BUILD_PYTHON=OFF" ./build.sh
```

```bat
set BUILD_WHEEL=0
set RUN_TESTS=0
set CMAKE_EXTRA_ARGS=-DDICOM_BUILD_PYTHON=OFF
call build.bat dicomsdl
```

## 按操作系统区分的选项

### macOS / `build.sh`

- `MACOSX_DEPLOYMENT_TARGET=...`: 覆盖默认 deployment target
- `CMAKE_OSX_ARCHITECTURES=...`: 显式选择 target architecture
- `ARCHFLAGS=...`: 在 `CMAKE_OSX_ARCHITECTURES` 未设置时影响 wheel build 的 architecture 检测

### Linux / `build.sh`

- `CMAKE_GENERATOR=...`: 强制指定 generator，而不是使用脚本自动选择
- `BUILD_PARALLELISM=N`: 当你想显式限制 CPU 使用量时很有用

### Windows / `build.bat`

- `DICOMSDL_WINDOWS_TOOLCHAIN=auto|msvc|clangcl|clang64`: 选择 Windows toolchain
- `CMAKE_GENERATOR=...`: 覆盖脚本选定的 generator
- `DICOMSDL_MSVC_ENABLE_LTCG=ON|OFF`: 切换 MSVC link-time code generation
- `DICOMSDL_MSVC_PGO=OFF|GEN|USE`: 控制 MSVC PGO mode
- `DICOMSDL_MSVC_PGO_DIR=...`: `.pgd/.pgc` profile data 目录
- `FORCE_WHEEL_RELEASE=1|0`: 将 wheel build 固定为 `Release`，或允许 non-Release build
- `VALIDATE_WHEEL_RUNTIME=1|0`: 启用或跳过 Windows wheel runtime validation

## Python source build

如果你需要本地 wheel、接近 editable 的开发流程，或自定义 Python 构建环境，请使用 [Build Python From Source](../developer/build_python_from_source.md)。

## 相关文档

- [Quickstart](quickstart.md)
- [Python DataSet Guide](python_dataset_guide.md)
- [Build Python From Source](../developer/build_python_from_source.md)
