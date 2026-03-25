# Build C++ From Source

Use this page when you are building DicomSDL from a repository checkout for
C++ development, example binaries, toolchain benchmarking, or codec/plugin
experiments.

## When to use this path

- You are working on the C++ core itself
- You want the example binaries such as `dicomdump` and `dicomconv`
- You need explicit Windows toolchain selection
- You want to test codec plugin modes or CMake toggles directly

## Minimal configure and build

```bash
cmake -S . -B build -DDICOM_BUILD_EXAMPLES=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

Notes:

- `FetchContent` resolves third-party dependencies during configure.
- `DICOM_BUILD_EXAMPLES=ON` builds the example binaries under `build/`.
- `Ninja` is recommended, but only required by some Windows toolchains.

## Unix-like helper script

On macOS and Linux you can also use the repository helper:

```bash
./build.sh
```

For the higher-level install/build decision tree, see [Installation](../guide/installation.md).

## Windows `build.bat` toolchain selection

`build.bat` supports:

- `MSVC`
- `clang-cl` with the MSVC runtime
- `MSYS2 clang64`

Select the toolchain with `DICOMSDL_WINDOWS_TOOLCHAIN`:

```bat
:: auto (default): prefer MSVC, then clang-cl, then clang64
set DICOMSDL_WINDOWS_TOOLCHAIN=auto
build

:: force MSVC
set DICOMSDL_WINDOWS_TOOLCHAIN=msvc
set BUILD_DIR=build-msvc
build

:: force clang-cl
set DICOMSDL_WINDOWS_TOOLCHAIN=clangcl
set CMAKE_GENERATOR=Ninja
set BUILD_DIR=build-clangcl
build

:: force MSYS2 clang64
set DICOMSDL_WINDOWS_TOOLCHAIN=clang64
set BUILD_DIR=build-clang64
build
```

If you switch generator or toolchain while reusing the same `BUILD_DIR`, reset
the CMake cache first:

```bat
set RESET_CMAKE_CACHE=1
```

## MSVC LTCG toggle

`DICOMSDL_MSVC_ENABLE_LTCG` controls MSVC whole-program optimization flags
(`/GL`, `/LTCG`) for `Release` and `RelWithDebInfo` builds. The default is `ON`.

```bat
set DICOMSDL_MSVC_ENABLE_LTCG=OFF
build
```

```bash
DICOMSDL_MSVC_ENABLE_LTCG=OFF ./build.sh
```

This toggle also applies to wheel builds.

## MSVC PGO

MSVC PGO is controlled with:

- `DICOMSDL_MSVC_PGO=OFF|GEN|USE`
- `DICOMSDL_MSVC_PGO_DIR=...`

`GEN` builds with profile instrumentation. `USE` consumes that profile data.

```bat
:: 1) Instrumented build
set DICOMSDL_MSVC_PGO=GEN
set DICOMSDL_MSVC_PGO_DIR=C:\Lab\workspace\test.git\build-pgo\msvc
build-wheel-static.bat

:: 2) Run a representative workload to produce .pgc/.pgd

:: 3) Profile-use build
set DICOMSDL_MSVC_PGO=USE
build-wheel-static.bat
```

Notes:

- `GEN` and `USE` require `DICOMSDL_MSVC_ENABLE_LTCG=ON`.
- `USE` fails fast if `${DICOMSDL_MSVC_PGO_DIR}/dicomsdl.pgd` is missing.

## MSYS2 clang64 prerequisites

Run the following in an `MSYS2 clang64` shell:

```bash
pacman -Syu
pacman -Su --noconfirm
pacman -S --needed --noconfirm \
  mingw-w64-clang-x86_64-clang \
  mingw-w64-clang-x86_64-llvm \
  mingw-w64-clang-x86_64-cmake \
  mingw-w64-clang-x86_64-ninja \
  mingw-w64-clang-x86_64-python \
  mingw-w64-clang-x86_64-python-pip \
  mingw-w64-clang-x86_64-pkgconf \
  mingw-w64-clang-x86_64-zlib \
  mingw-w64-clang-x86_64-libtiff \
  mingw-w64-clang-x86_64-lcms2 \
  git
```

Then make sure `clang64` tools come first on `PATH` before running `build.bat`:

```bat
set PATH=C:\msys64\clang64\bin;%PATH%
set DICOMSDL_WINDOWS_TOOLCHAIN=clang64
set BUILD_DIR=build-clang64
build
```

## Visual Studio `clang-cl` prerequisites

Install the Visual Studio C++ workload and `clang-cl`, then build from a
Developer Command Prompt:

```bat
where clang-cl
where cl
where link

set DICOMSDL_WINDOWS_TOOLCHAIN=clangcl
set CMAKE_GENERATOR=Ninja
set BUILD_DIR=build-clangcl
build
```

`clangcl` mode uses `clang-cl` as the compiler with the MSVC toolchain/runtime.

## Codec mode overrides

Both `build.sh` and `build.bat` support per-codec mode selection with:

- `DICOMSDL_PIXEL_DEFAULT_MODE` (`builtin|shared|none`, default: `builtin`)
- `DICOMSDL_PIXEL_JPEG_MODE`
- `DICOMSDL_PIXEL_JPEGLS_MODE`
- `DICOMSDL_PIXEL_JPEG2K_MODE`
- `DICOMSDL_PIXEL_HTJ2K_MODE`
- `DICOMSDL_PIXEL_JPEGXL_MODE`

When one or more codec modes are set to `shared`, CMake builds per-codec shared
plugins:

- Windows: `dicomsdl_pixel_*_plugin.dll`
- Linux: `libdicomsdl_pixel_*_plugin.so`
- macOS: `libdicomsdl_pixel_*_plugin.dylib`

Examples:

```bash
# Disable all codecs, then enable JPEG2K as a shared plugin only
DICOMSDL_PIXEL_DEFAULT_MODE=none \
DICOMSDL_PIXEL_JPEG2K_MODE=shared \
BUILD_DIR=build-codec-shared \
BUILD_WHEEL=0 RUN_TESTS=0 \
./build.sh
```

```bat
:: builtin JPEG2K, shared JPEGXL, disable HTJ2K
set DICOMSDL_PIXEL_JPEG2K_MODE=builtin
set DICOMSDL_PIXEL_JPEGXL_MODE=shared
set DICOMSDL_PIXEL_HTJ2K_MODE=none
set BUILD_DIR=build-codec-mix
build
```

Optional extra CMake flags can be appended with `CMAKE_EXTRA_ARGS`:

```bat
set CMAKE_EXTRA_ARGS=-DDICOMSDL_PIXEL_OPENJPEG_STATIC_PLUGIN=ON -DDICOMSDL_PIXEL_OPENJPEG_PLUGIN=OFF -DDICOMSDL_PIXEL_JPEGXL_STATIC_PLUGIN=OFF -DDICOMSDL_PIXEL_JPEGXL_PLUGIN=ON -DDICOMSDL_ENABLE_JPEGXL=ON
build
```

```bash
CMAKE_EXTRA_ARGS="-DDICOMSDL_PIXEL_OPENJPEG_STATIC_PLUGIN=ON -DDICOMSDL_PIXEL_OPENJPEG_PLUGIN=OFF" ./build.sh
```

## Run C++ examples

```bash
# Keyword -> (Tag, VR)
./build/keyword_lookup_example PatientName

# Tag -> keyword/name metadata
./build/tag_lookup_example "(0010,0010)"

# UID keyword/value -> registry entry
./build/uid_lookup_example ExplicitVRLittleEndian

# Dump one or more DICOM files
./build/dicomdump sample.dcm
./build/dicomdump a.dcm b.dcm

# Change transfer syntax and write a new file
./build/dicomconv in.dcm out.dcm ExplicitVRLittleEndian
./build/dicomconv in.dcm out.dcm jpeg --quality 92
```

For full CLI behavior, see [CLI Tools](../guide/cli_tools.md).

## Related docs

- [Installation](../guide/installation.md)
- [Quickstart](../guide/quickstart.md)
- [Build Python From Source](build_python_from_source.md)
- [WG04 Windows Toolchain Benchmark](windows_wg04_toolchain_benchmark.md)
