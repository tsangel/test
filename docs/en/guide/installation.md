# Installation

Use this page when you want a repeatable install/build path and a clearer view
of the environments dicomsdl currently targets.

## Environment summary

### Python / PyPI

- CPython `3.9` to `3.14`
- Current wheel CI targets:
  - Linux `x86_64`
  - macOS `x86_64`
  - macOS `arm64`
  - Windows `AMD64`
- `musllinux` wheels are not currently built
- Base install is enough for metadata access, file I/O, and transcode workflows
- Optional extras are available for NumPy-backed pixel helpers and Pillow preview

### C++ / source build

- `git`
- `CMake 3.16+`
- `C++20` compiler
- `Ninja` is recommended, but only required by some Windows toolchains
- Python is only required when you build the Python binding or wheel

### Windows toolchains

`build.bat` documents and supports these source-build paths:

- `MSVC` (`cl.exe`)
- `clang-cl` with `Ninja`
- `MSYS2 clang64` with `Ninja`

If `DICOMSDL_WINDOWS_TOOLCHAIN` is not set, `build.bat` auto-selects
`msvc`, then `clangcl`, then `clang64` based on what is available on `PATH`.

### macOS deployment target

- `x86_64` builds default to macOS `10.15`
- `arm64` builds default to macOS `11.0`
- Set `MACOSX_DEPLOYMENT_TARGET` only when you need explicit control

## Choose a path

- PyPI install: fastest way to try the Python binding
- C++ build from a checkout: best path when you are integrating dicomsdl into your C++ application
- Unix-like `build.sh`: convenience wrapper for macOS / Linux builds
- Python source build from a checkout: documented in [Build Python From Source](../developer/build_python_from_source.md)
- Windows `build.bat`: preferred entry point when you need explicit toolchain selection

## PyPI install

```bash
python -m pip install --upgrade pip
pip install dicomsdl
```

If `pip` falls back to building from source on your platform, install `cmake`
first.

Optional Python extras:

- `pip install "dicomsdl[numpy]"` for `to_array(...)`, `decode_into(...)`, and NumPy-backed pixel helpers
- `pip install "dicomsdl[numpy,pil]"` if you also want `to_pil_image(...)` convenience helpers (`numpy` + `Pillow`)

If you only need server-side metadata access, file I/O, or transcode workflows,
the base `pip install dicomsdl` path is enough.

## C++ build from a checkout

```bash
git clone https://github.com/tsangel/dicomsdl.git
cd dicomsdl
cmake -S . -B build -DDICOM_BUILD_EXAMPLES=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

On Linux and macOS, this is the standard CMake path. On Windows, prefer
`build.bat` if you are not already pinned to a specific generator/toolchain.

## Unix-like `build.sh`

For macOS and Linux, the wrapper script is:

```bash
./build.sh
```

To build only selected targets:

```bash
./build.sh dicomsdl
./build.sh dicomsdl _dicomsdl
```

Trailing arguments are forwarded as `cmake --build --target ...` targets.

## Windows `build.bat`

The shortest Windows entry point is:

```bat
call build.bat dicomsdl
```

If you want to force a specific toolchain:

```bat
set DICOMSDL_WINDOWS_TOOLCHAIN=msvc
call build.bat dicomsdl
```

Replace `msvc` with `clangcl` or `clang64` when needed.

Use an initialized Visual Studio developer environment for `MSVC` and
`clang-cl`. For `clang64`, run inside an MSYS2 `clang64` environment with
`clang`, `clang++`, `cmake`, and `ninja` available on `PATH`.

Like `build.sh`, trailing arguments are forwarded as `cmake --build --target ...`
targets.

## Common build-script options

Both `build.sh` and `build.bat` honor the same core environment variables:

- `BUILD_DIR`: build directory path
- `BUILD_TYPE`: build configuration such as `Release` or `Debug`
- `BUILD_TESTING=ON|OFF`: enable or disable CTest targets
- `DICOM_BUILD_EXAMPLES=ON|OFF`: build example binaries
- `RUN_TESTS=1|0`: run or skip CTest after build
- `BUILD_WHEEL=1|0`: build or skip the Python wheel step
- `WHEEL_ONLY=1`: skip top-level CMake configure/build and build only the wheel
- `WHEEL_DIR=...`: output directory for built wheels
- `PYTHON_BIN=...`: Python interpreter used for wheel builds
- `CMAKE_EXTRA_ARGS="..."`: append raw CMake configure arguments
- `BUILD_PARALLELISM=N`: override auto-detected parallel build count
- `RESET_CMAKE_CACHE=1`: clear stale CMake cache when generator/compiler settings changed
- `DICOMSDL_PIXEL_DEFAULT_MODE=builtin|shared|none`: default codec plugin mode
- `DICOMSDL_PIXEL_<CODEC>_MODE=builtin|shared|none`: per-codec override for `JPEG`, `JPEGLS`, `JPEG2K`, `HTJ2K`, `JPEGXL`

Examples:

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

## OS-specific options

### macOS / `build.sh`

- `MACOSX_DEPLOYMENT_TARGET=...`: override the default deployment target
- `CMAKE_OSX_ARCHITECTURES=...`: choose target architectures explicitly
- `ARCHFLAGS=...`: influences wheel-build architecture detection when `CMAKE_OSX_ARCHITECTURES` is unset

### Linux / `build.sh`

- `CMAKE_GENERATOR=...`: force a generator instead of the script's auto-selection
- `BUILD_PARALLELISM=N`: useful when you want to cap CPU usage explicitly

### Windows / `build.bat`

- `DICOMSDL_WINDOWS_TOOLCHAIN=auto|msvc|clangcl|clang64`: choose the Windows toolchain
- `CMAKE_GENERATOR=...`: override the generator chosen by the script
- `DICOMSDL_MSVC_ENABLE_LTCG=ON|OFF`: toggle MSVC link-time code generation
- `DICOMSDL_MSVC_PGO=OFF|GEN|USE`: control MSVC PGO mode
- `DICOMSDL_MSVC_PGO_DIR=...`: directory for `.pgd/.pgc` profile data
- `FORCE_WHEEL_RELEASE=1|0`: keep wheel builds pinned to `Release` or allow non-Release builds
- `VALIDATE_WHEEL_RUNTIME=1|0`: enable or skip Windows wheel runtime validation

## Python source build

If you need a local wheel, editable-like development flow, or a custom Python
build environment, use [Build Python From Source](../developer/build_python_from_source.md).

## Related docs

- [Quickstart](quickstart.md)
- [Python DataSet Guide](python_dataset_guide.md)
- [Build Python From Source](../developer/build_python_from_source.md)
