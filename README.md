# dicomsdl

Minimal DICOM file wrapper with optional Python bindings.

## Repository Setup

### Full setup (default)

```bash
git submodule update --init --recursive
```

### JPEG-XL minimal setup (optional)

If you want to work on JPEG-XL integration without downloading all nested `extern/libjxl`
submodules, use:

```bash
# macOS / Linux
./scripts/setup_libjxl_minimal.sh
```

```bat
:: Windows (cmd.exe)
scripts\setup_libjxl_minimal.bat
```

The minimal script initializes only:

- `extern/libjxl/third_party/highway`
- `extern/libjxl/third_party/brotli`
- `extern/libjxl/third_party/skcms`

To enable JPEG-XL dependency build in this project:

```bash
cmake -S . -B build -DDICOMSDL_ENABLE_JPEGXL=ON
```

## JPEG-LS (CharLS) Pin

`extern/charls` is intentionally pinned to `2.2.1` for now.
Newer CharLS tags (`2.3.0+`) show a regression for WG04 `MR1_JLSL`
codestream decoding in this project. See:
`misc/repro/charls_mr1_jpegls_regression/`.

By default, CMake enforces the pinned CharLS submodule revision.
If you explicitly need to test another CharLS revision, configure with:

```bash
cmake -S . -B build -DDICOMSDL_ALLOW_UNPINNED_CHARLS=ON
```

## JPEG (libjpeg-turbo)

`extern/libjpeg-turbo` is built as a static dependency and linked into `dicomsdl`.

- JPEG Lossless SV1 (`1.2.840.10008.1.2.4.70`) is supported in the current decoder path.
- JPEG Extended 12-bit (Process 2/4, `1.2.840.10008.1.2.4.51`) WG04 codestreams are
  decoded with a compatibility fix for malformed `SOF1 + SOS(Se=0)` headers.

## C++ Build

```
cmake -S . -B build -DDICOM_BUILD_EXAMPLES=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Windows `build.bat` toolchain selection

`build.bat` supports `MSVC`, `clang-cl (MSVC runtime)`, and `MSYS2 clang64` via
`DICOMSDL_WINDOWS_TOOLCHAIN`.

```cmd
:: auto (default): prefer MSVC when cl.exe exists, then clang-cl, then clang64
set DICOMSDL_WINDOWS_TOOLCHAIN=auto
build

:: force MSVC (Developer Command Prompt or vcvarsall.bat environment)
set DICOMSDL_WINDOWS_TOOLCHAIN=msvc
set BUILD_DIR=build-msvc
build

:: force clang-cl (Developer Command Prompt + Ninja)
set DICOMSDL_WINDOWS_TOOLCHAIN=clangcl
set CMAKE_GENERATOR=Ninja
set BUILD_DIR=build-clangcl
build

:: force MSYS2 clang64 (clang/clang++/ninja on PATH)
set DICOMSDL_WINDOWS_TOOLCHAIN=clang64
set BUILD_DIR=build-clang64
build
```

If you switch generator/toolchain while reusing the same `BUILD_DIR`, set:

```cmd
set RESET_CMAKE_CACHE=1
```

### MSYS2 clang64 prerequisites

Run the following in an `MSYS2 clang64` shell:

```bash
pacman -Syu
# reopen the clang64 shell once after the first full upgrade, then:
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

Then in `cmd.exe` (or PowerShell), put clang64 tool binaries on `PATH` before running
`build.bat`:

```cmd
set PATH=C:\msys64\clang64\bin;%PATH%
set DICOMSDL_WINDOWS_TOOLCHAIN=clang64
set BUILD_DIR=build-clang64
build
```

### Visual Studio clang-cl prerequisites

Install Visual Studio C++ workload and clang-cl tools, then run from a
Developer Command Prompt (`x64 Native Tools Command Prompt`):

```cmd
where clang-cl
where cl
where link

set DICOMSDL_WINDOWS_TOOLCHAIN=clangcl
set CMAKE_GENERATOR=Ninja
set BUILD_DIR=build-clangcl
build
```

`clangcl` mode uses `clang-cl` as compiler with the MSVC toolchain/runtime.

### Codec mode overrides (`build.sh` / `build.bat`)

Both scripts support per-codec mode selection with:

- `DICOMSDL_PIXEL_DEFAULT_MODE` (`builtin|shared|none`, default: `builtin`)
- `DICOMSDL_PIXEL_JPEG_MODE`
- `DICOMSDL_PIXEL_JPEGLS_MODE`
- `DICOMSDL_PIXEL_JPEG2K_MODE`
- `DICOMSDL_PIXEL_HTJ2K_MODE`
- `DICOMSDL_PIXEL_JPEGXL_MODE`

`DICOMSDL_PIXEL_*_MODE` controls pixel v2 plugin toggles (`DICOMSDL_PIXEL_*`).
Legacy `DICOMSDL_CODEC_*` CMake options are removed.

When one or more codec modes are set to `shared`, CMake builds per-codec shared
plugins:

- Windows: `dicomsdl_pixel_*_plugin.dll`
- Linux: `libdicomsdl_pixel_*_plugin.so`
- macOS: `libdicomsdl_pixel_*_plugin.dylib`

When wheel build is enabled, `setup.py` bundles the produced shared plugin
libraries into the `dicomsdl/` package:

- Windows: `dicomsdl_pixel_*_plugin.dll`
- Linux: `libdicomsdl_pixel_*_plugin.so`
- macOS: `libdicomsdl_pixel_*_plugin.dylib`

The Python package auto-loads bundled codec plugins at import time by default.
Set `DICOMSDL_AUTOLOAD_BUNDLED_CODECS=0` to disable auto-loading.

Examples:

```bash
# build.sh: disable all codecs, then enable JPEG2K as shared plugin only
DICOMSDL_PIXEL_DEFAULT_MODE=none \
DICOMSDL_PIXEL_JPEG2K_MODE=shared \
BUILD_DIR=build-codec-shared \
BUILD_WHEEL=0 RUN_TESTS=0 \
./build.sh
```

```cmd
:: build.bat: builtin JPEG2K, shared JPEGXL, disable HTJ2K
set DICOMSDL_PIXEL_JPEG2K_MODE=builtin
set DICOMSDL_PIXEL_JPEGXL_MODE=shared
set DICOMSDL_PIXEL_HTJ2K_MODE=none
set BUILD_DIR=build-codec-mix
build
```

Optional extra CMake configure flags can be appended with:

```cmd
set CMAKE_EXTRA_ARGS=-DDICOMSDL_PIXEL_OPENJPEG_STATIC_PLUGIN=ON -DDICOMSDL_PIXEL_OPENJPEG_PLUGIN=OFF -DDICOMSDL_PIXEL_JPEGXL_STATIC_PLUGIN=OFF -DDICOMSDL_PIXEL_JPEGXL_PLUGIN=ON -DDICOMSDL_ENABLE_JPEGXL=ON
build
```

```bash
CMAKE_EXTRA_ARGS="-DDICOMSDL_PIXEL_OPENJPEG_STATIC_PLUGIN=ON -DDICOMSDL_PIXEL_OPENJPEG_PLUGIN=OFF" ./build.sh
```

### Run C++ examples

```
# Keyword -> (Tag, VR)
./build/keyword_lookup_example PatientName

# Tag -> keyword/name metadata
./build/tag_lookup_example (0010,0010)

# UID keyword/value -> registry entry
./build/uid_lookup_example ExplicitVRLittleEndian

# Dump one or more DICOM files
./build/dicomdump path/to/file.dcm
./build/dicomdump path/to/a.dcm path/to/b.dcm
./build/dicomdump --no-offset --max-print-chars 120 path/to/file.dcm

# Change Transfer Syntax UID and write to a new file
./build/dicomconv input.dcm output.dcm ExplicitVRLittleEndian
./build/dicomconv input.dcm output.dcm 1.2.840.10008.1.2
./build/dicomconv input.dcm output.dcm jpeg --quality 92
./build/dicomconv input.dcm output.dcm jpeg2k --target-psnr 45 --threads -1
./build/dicomconv input.dcm output.dcm htj2k-lossless --no-color-transform
./build/dicomconv input.dcm output.dcm jpegxl --distance 1.5 --effort 7 --threads -1
./build/dicomconv input.dcm output.dcm jpegxl-lossless
```

### Preferred C++ DataSet access pattern

```cpp
using namespace dicom::literals;
auto file = dicom::read_file("sample.dcm");
auto& dataset = file->dataset();

long row_count = dataset["Rows"_tag].to_long().value_or(1);
```

- Prefer `dataset[tag].to_xxx().value_or(default)` for user-facing reads with defaults.
- Use `if (auto& e = dataset[tag]; e)` only when missing/present distinction matters.
- Keep `get_dataelement(...)` for low-level pointer workflows and tag-path parsing.

## Python Wheel

Python ≥ 3.9 is required.

```
python -m pip install --upgrade pip
pip install cmake
pip wheel . --no-build-isolation --no-deps -w dist
```

After building, install the wheel:

```
pip install dist/dicomsdl-*.whl
```

### Static wheel wrappers (`build-wheel-static.bat` / `build-wheel-static.sh`)

Use the static wrapper scripts when you want a clean static wheel build pipeline.

```bash
# macOS / Linux
./build-wheel-static.sh
```

```cmd
:: Windows (cmd.exe)
build-wheel-static.bat
```

Default behavior:

- Pre-clean old outputs before build:
  - `${BUILD_DIR}` (default: `build-wheel-static`)
  - `build/temp.*`, `build/lib.*`, `build/bdist.*`
  - `${WHEEL_DIR}` (default: `dist-static`)
- Force wheel build to Release (`FORCE_WHEEL_RELEASE=1`, `BUILD_TYPE=Release`).
- Run `build.sh` / `build.bat` to produce a new wheel.
- Force-reinstall the newest wheel from `${WHEEL_DIR}` after a successful build.

Useful toggles:

- `STATIC_PRE_CLEAN_OUTPUTS=0`: skip pre-clean stage.
- `INSTALL_BUILT_WHEEL=0`: skip automatic wheel install.
- `FORCE_WHEEL_RELEASE=0`: allow non-Release wheel builds.

### Run Python examples

```
python examples/python/keyword_lookup_example.py PatientName Rows
python examples/python/tag_lookup_example.py 00100010 (0008,0016)
python examples/python/uid_lookup_example.py ExplicitVRLittleEndian 1.2.840.10008.1.2.1
python examples/python/dump_dataset_example.py path/to/file.dcm
python examples/python/dump_dataset_example.py path/to/file.dcm --raw-preview 16
python examples/python/raw_value_span_example.py path/to/file.dcm PixelData
python examples/python/pixel_decode_safe_example.py path/to/file.dcm --frame 0
python examples/python/dicomconv_example.py input.dcm output.dcm ExplicitVRLittleEndian
python examples/python/dicomconv_example.py input.dcm output.dcm jpeg --quality 92
```

### Run `dicomdump` CLI

`dicomsdl` wheel 설치 후 `dicomdump` 스크립트를 사용할 수 있습니다.

```bash
# Basic dump
dicomdump path/to/file.dcm

# Multiple files (each output line is prefixed with "filename:")
dicomdump path/to/a.dcm path/to/b.dcm
dicomdump *.dcm

# Hide OFFSET column
dicomdump path/to/file.dcm --no-offset

# Control truncation width
dicomdump path/to/file.dcm --max-print-chars 120
```

### Run `dicomconv` CLI

`dicomsdl` wheel 설치 후 `dicomconv` 스크립트를 사용할 수 있습니다.

```bash
# Change transfer syntax by keyword
dicomconv input.dcm output.dcm ExplicitVRLittleEndian

# Change transfer syntax by dotted UID value
dicomconv input.dcm output.dcm 1.2.840.10008.1.2

# JPEG (baseline), JPEG2000, HTJ2K shortcuts
dicomconv input.dcm output.dcm jpeg --quality 92
dicomconv input.dcm output.dcm jpeg2k --target-psnr 45 --threads -1
dicomconv input.dcm output.dcm htj2k-lossless --no-color-transform
dicomconv input.dcm output.dcm jpegxl --distance 1.5 --effort 7 --threads -1
dicomconv input.dcm output.dcm jpegxl-lossless

# Show full help (all options + examples)
dicomconv -h
```

Python 코드에서는 일관되게 `import dicomsdl as dicom` 형식의 alias를 사용합니다.

### Pixel Decode Safety Warning (v0.1.6 policy)

pixel decode 경로는 내부적으로 pixel metadata를 캐시할 수 있습니다.
아래 항목이 바뀌면 이전 metadata/shape/stride 가정은 무효로 간주해야 합니다.

- `TransferSyntaxUID`
- `Rows`, `Columns`, `SamplesPerPixel`, `BitsAllocated`
- `PixelRepresentation`, `PlanarConfiguration`, `NumberOfFrames`
- `PixelData`, `FloatPixelData`, `DoubleFloatPixelData`

권장 사항:

- 변경 이후에는 반드시 최신 pixel metadata를 다시 조회합니다.
- 기존 decode 출력 버퍼(`decode_into`의 `out`)는 재사용하지 말고 다시 할당합니다.
- C++에서는 `pixel_info(true)` 또는 새로 로드한 객체를 사용합니다.

### Run Python Tests

```bash
python -m venv .venv && source .venv/bin/activate
python -m pip install --upgrade pip cmake pytest
pip install -e .
pytest -q tests/python
```

Windows에서는 PowerShell/`cmd`에서 가상환경 활성화만 플랫폼에 맞게 바꿔주면 동일합니다.

### Update Python Stub Snapshot

`nanobind` 기반 자동 생성 스텁 스냅샷은 아래 명령으로 갱신합니다.

```bash
# Build Python extension first (example: default build dir)
cmake --build build --target _dicomsdl

# Regenerate snapshot
scripts/update_stub.sh --build-dir build

# Check-only mode (used in CI)
scripts/update_stub.sh --check --build-dir build
```

### WG04 Pixel Decode Benchmark

WG04 샘플(`REF`, `RLE`, `J2KR`, `J2KI`, `JLSL`, `JLSN`, `JPLL`, `JPLY`)을 codec별로
pixel decode 벤치마크할 수 있습니다.

```bash
export DICOMSDL_WG04_IMAGES_BASE=/Users/tsangel/workspace.dev/sample/nema/WG04/IMAGES
python benchmarks/python/benchmark_wg04_pixel_decode.py --warmup 1 --repeat 5
```

기본 `dicomsdl` 경로(`to_array`)는 현재 JPEG 2000 디코드에서
`decoder_threads=-1`(all CPUs auto)을 사용합니다.

`dicomsdl` vs `pydicom` 비교 테이블:

```bash
python benchmarks/python/benchmark_wg04_pixel_decode.py --backend both --warmup 1 --repeat 5
```

`dicomsdl`에서 출력 버퍼 재사용(`decode_into`) 모드:

```bash
python benchmarks/python/benchmark_wg04_pixel_decode.py --backend dicomsdl --reuse-output --repeat 10
```

`--reuse-output` 경로도 기본 thread hint는 `threads=-1`입니다.

`pydicom`에서 출력 버퍼 재사용 모드(비압축은 `numpy_handler(read_only)+copyto`,
압축은 `pixel_array+copyto` fallback):

```bash
python benchmarks/python/benchmark_wg04_pixel_decode.py --backend pydicom --reuse-output-pydicom --repeat 10
```

특정 codec만 실행:

```bash
python benchmarks/python/benchmark_wg04_pixel_decode.py --codec JLSL --codec JLSN --repeat 10
```

JSON 리포트 저장:

```bash
python benchmarks/python/benchmark_wg04_pixel_decode.py --backend both --json build/wg04_pixel_decode_bench.json
```

최신 스냅샷(2026-02-20, `--backend both --warmup 1 --repeat 3`)에서는
`TOTAL` 기준 `dicomsdl 19.680 ms/decode` vs `pydicom 45.487 ms/decode`로
약 `2.31x` (`dcm/pyd x`)를 기록했습니다.
상세 표는 `docs/pydicom_pixel_decoding_wg04.md`, 원본 수치는
`build/wg04_pixel_decode_compare_r3.json`에서 확인할 수 있습니다.

## Python Wheel Quick Commands

### macOS / Linux

```bash
python -m venv .venv && source .venv/bin/activate
python -m pip install --upgrade pip cmake
pip wheel . --no-build-isolation --no-deps -w dist
pip install --force-reinstall dist/dicomsdl-*.whl
python -c "import dicomsdl as dicom; tag, vr = dicom.keyword_to_tag_vr('PatientName'); print(int(tag), vr.str())"
python -c "import dicomsdl as dicom; df = dicom.read_file('sample.dcm'); print(df.path, type(df.dataset).__name__)"
python -c "import dicomsdl as dicom; data = b'DICM'; df = dicom.read_bytes(data, name='inline'); print(df.path, len(data))"
```

### Windows (PowerShell)

```powershell
py -m venv .venv; .\.venv\Scripts\Activate.ps1
python -m pip install --upgrade pip cmake
pip wheel . --no-build-isolation --no-deps -w dist
pip install --force-reinstall dist\dicomsdl-*.whl
python -c "import dicomsdl as dicom; tag, vr = dicom.keyword_to_tag_vr('PatientName'); print(int(tag), vr.str())"
```

### Windows (cmd.exe)

```cmd
py -m venv .venv && .\.venv\Scripts\activate
python -m pip install --upgrade pip cmake
pip wheel . --no-build-isolation --no-deps -w dist
pip install --force-reinstall dist\dicomsdl-*.whl
python -c "import dicomsdl as dicom; tag, vr = dicom.keyword_to_tag_vr('PatientName'); print(int(tag), vr.str())"
```

## Continuous Integration

GitHub Actions builds Python wheels for Linux, macOS (x86_64 & arm64), and Windows
across CPython 3.9 through 3.14. Generated wheels are published as workflow
artifacts.
