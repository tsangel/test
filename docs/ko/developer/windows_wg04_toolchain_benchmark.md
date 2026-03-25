# Windows WG04 Toolchain Benchmark Repro Guide

```{note}
이 페이지 본문은 아직 영어 원문입니다. 필요하면 영문 페이지를 기준으로 읽어 주세요.
```

This guide documents how to reproduce WG04 pixel decode benchmarks on Windows for:

- `MSYS2 clang64`
- `MSYS2 ucrt64`
- `MSVC 2026` (LTCG ON)
- `MSVC 2026` (LTCG OFF)
- `clang-cl` (VS LLVM)

The commands below are written so another Windows machine can run the same workflow without changing project source code.

## 1) Required Paths and Tools

Use these paths (or keep equivalent values consistently):

- Repository: `C:\Lab\workspace\test.git`
- WG04 images root: `C:\Lab\img\WG04\IMAGES`
- MSYS2 root: `C:\msys64`
- VS vcvars script: `C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat`

Required software:

- MSYS2 (`clang64`, `ucrt64` shells)
- Visual Studio 2026 C++ toolchain (MSVC + clang-cl)
- Python (for VS builds: `py -3`)

## 2) Hardware Baseline (CPU / RAM / Power)

Benchmark comparisons are sensitive to host specs. Record CPU/RAM/power profile before running.

Capture hardware snapshot:

```powershell
$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1 Name,NumberOfCores,NumberOfLogicalProcessors,MaxClockSpeed
$ram = (Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory
[PSCustomObject]@{
  CPU = $cpu.Name
  Cores = $cpu.NumberOfCores
  LogicalProcessors = $cpu.NumberOfLogicalProcessors
  MaxClockMHz = $cpu.MaxClockSpeed
  RAM_GiB = [math]::Round($ram / 1GB, 2)
} | Format-List
```

Capture current power plan:

```powershell
powercfg /L
powercfg /GETACTIVESCHEME
```

Example host used for the benchmark runs in this repository session:

- CPU: `12th Gen Intel(R) Core(TM) i9-12900K`
- Cores / Threads: `16 / 24`
- RAM: `127.72 GiB` (`137133056000` bytes)

## 3) Compiler Settings and Options

Common settings:

- CMake build type: `Release`
- Benchmark backend: DicomSDL
- Benchmark options: `--warmup 2 --repeat 10`

Compiler selection by toolchain:

| Toolchain | Compiler | Generator | Main environment settings |
|---|---|---|---|
| MSYS2 `clang64` | `clang` / `clang++` | `Ninja` | `PATH=C:\msys64\clang64\bin;C:\msys64\usr\bin;...`, `CC=clang`, `CXX=clang++`, `CMAKE_GENERATOR=Ninja` |
| MSYS2 `ucrt64` | `gcc` / `g++` | `Ninja` | `PATH=C:\msys64\ucrt64\bin;C:\msys64\usr\bin;...`, `CC=gcc`, `CXX=g++`, `CMAKE_GENERATOR=Ninja` |
| MSVC 2026 | `cl.exe` | Visual Studio generator (from CMake default) | `vcvars64.bat` loaded |
| `clang-cl` | `clang-cl` | `Ninja` | `vcvars64.bat` loaded + `DICOMSDL_CMAKE_ARGS=-DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_RC_COMPILER=llvm-rc` |

Observed Release optimization flags from CMake cache / generated build files:

- MSYS2 (`clang64`, `ucrt64`):
  - `CMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG`
  - `CMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG`
- MSVC (`cl.exe`) and `clang-cl`:
  - `CMAKE_C_FLAGS_RELEASE=/O2 /Ob2 /DNDEBUG`
  - `CMAKE_CXX_FLAGS_RELEASE=/O2 /Ob2 /DNDEBUG` (plus `/GR /EHsc` in base CXX flags)
- `clang-cl` (Ninja) also showed MSVC-style optimization/runtime flags in generated rules such as:
  - `/Ox /Ob2 /Oi /Ot /DNDEBUG /MD`

LTCG toggle behavior:

- `DICOMSDL_MSVC_ENABLE_LTCG=ON`:
  - VS project files include `WholeProgramOptimization=true`
  - Link setting includes `LinkTimeCodeGeneration=UseLinkTimeCodeGeneration`
- `DICOMSDL_MSVC_ENABLE_LTCG=OFF`:
  - those LTCG settings are disabled

Optional verification commands:

```powershell
# Compiler versions
clang --version
gcc --version
clang-cl --version
cmd /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cl'

# CMake Release flags (example)
Select-String -Path .\build\temp*\*\CMakeCache.txt -Pattern '^CMAKE_(C|CXX)_FLAGS_RELEASE|^DICOMSDL_MSVC_ENABLE_LTCG'
```

## 4) Long Path Requirement (Important)

MSYS2 Python wheel builds may fail with deep `FetchContent` paths when Windows long paths are disabled.

Check:

```powershell
reg query HKLM\SYSTEM\CurrentControlSet\Control\FileSystem /v LongPathsEnabled
```

If `LongPathsEnabled` is `0x0`, enable and reboot:

```powershell
reg add HKLM\SYSTEM\CurrentControlSet\Control\FileSystem /v LongPathsEnabled /t REG_DWORD /d 1 /f
```

## 5) Install MSYS2 Packages

### 5.1 clang64 packages

```powershell
$env:MSYSTEM='CLANG64'
$env:CHERE_INVOKING='1'
& 'C:\msys64\usr\bin\bash.exe' -lc 'pacman -S --noconfirm --needed \
  mingw-w64-clang-x86_64-clang \
  mingw-w64-clang-x86_64-llvm \
  mingw-w64-clang-x86_64-cmake \
  mingw-w64-clang-x86_64-ninja \
  mingw-w64-clang-x86_64-python \
  mingw-w64-clang-x86_64-python-pip \
  mingw-w64-clang-x86_64-python-setuptools \
  mingw-w64-clang-x86_64-python-wheel \
  mingw-w64-clang-x86_64-python-numpy \
  mingw-w64-clang-x86_64-pkgconf \
  mingw-w64-clang-x86_64-zlib \
  mingw-w64-clang-x86_64-libtiff \
  mingw-w64-clang-x86_64-lcms2'
```

### 5.2 ucrt64 packages

```powershell
$env:MSYSTEM='UCRT64'
$env:CHERE_INVOKING='1'
& 'C:\msys64\usr\bin\bash.exe' -lc 'pacman -S --noconfirm --needed \
  mingw-w64-ucrt-x86_64-gcc \
  mingw-w64-ucrt-x86_64-clang \
  mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-python \
  mingw-w64-ucrt-x86_64-python-pip \
  mingw-w64-ucrt-x86_64-python-setuptools \
  mingw-w64-ucrt-x86_64-python-wheel \
  mingw-w64-ucrt-x86_64-python-numpy \
  mingw-w64-ucrt-x86_64-pkgconf \
  mingw-w64-ucrt-x86_64-zlib \
  mingw-w64-ucrt-x86_64-libtiff \
  mingw-w64-ucrt-x86_64-lcms2'
```

## 6) Build DicomSDL per Toolchain

All commands below run from repo root:

```powershell
Set-Location C:\Lab\workspace\test.git
```

### 6.1 MSYS2 clang64 wheel

```powershell
$env:PATH='C:\msys64\clang64\bin;C:\msys64\usr\bin;' + $env:PATH
$env:CMAKE_GENERATOR='Ninja'
$env:CC='clang'
$env:CXX='clang++'
python -m venv --clear --system-site-packages .venv-clang64
.\.venv-clang64\bin\python -m pip install --upgrade pip setuptools wheel
.\.venv-clang64\bin\python -m pip install --no-build-isolation --no-deps --force-reinstall .
```

### 6.2 MSYS2 ucrt64 wheel

```powershell
$env:PATH='C:\msys64\ucrt64\bin;C:\msys64\usr\bin;' + $env:PATH
$env:CMAKE_GENERATOR='Ninja'
$env:CC='gcc'
$env:CXX='g++'
python -m venv --clear --system-site-packages .venv-ucrt64
.\.venv-ucrt64\bin\python -m pip install --upgrade pip setuptools wheel
.\.venv-ucrt64\bin\python -m pip install --no-build-isolation --no-deps --force-reinstall .
```

### 6.3 MSVC 2026 (LTCG ON, default)

```powershell
cmd /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && ^
  cd /d C:\Lab\workspace\test.git && ^
  py -3 -m venv .venv-msvc && ^
  .venv-msvc\Scripts\python -m pip install --upgrade pip setuptools wheel numpy && ^
  .venv-msvc\Scripts\python -m pip install --no-build-isolation --no-deps --force-reinstall .'
```

### 6.4 clang-cl wheel

```powershell
cmd /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && ^
  cd /d C:\Lab\workspace\test.git && ^
  py -3 -m venv .venv-clangcl && ^
  .venv-clangcl\Scripts\python -m pip install --upgrade pip setuptools wheel numpy && ^
  set "CMAKE_GENERATOR=Ninja" && ^
  set "DICOMSDL_CMAKE_ARGS=-DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_RC_COMPILER=llvm-rc" && ^
  .venv-clangcl\Scripts\python -m pip install --no-build-isolation --no-deps --force-reinstall .'
```

### 6.5 MSVC 2026 (LTCG OFF)

```powershell
cmd /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && ^
  cd /d C:\Lab\workspace\test.git && ^
  py -3 -m venv .venv-msvc-ltcgoff && ^
  .venv-msvc-ltcgoff\Scripts\python -m pip install --upgrade pip setuptools wheel numpy && ^
  set "DICOMSDL_MSVC_ENABLE_LTCG=OFF" && ^
  .venv-msvc-ltcgoff\Scripts\python -m pip install --no-build-isolation --no-deps --force-reinstall .'
```

## 7) Run WG04 Benchmark (same settings for all)

Benchmark settings used for stable comparison:

- `--backend dicomsdl`
- `--warmup 2`
- `--repeat 10`

Note: current benchmark script removes hardcoded `sys.path` entries under `C:\Lab\workspace\test.git`. Keep this `PYTHONPATH` line exactly to avoid import errors.

### 7.1 clang64

```powershell
$env:PATH='C:\msys64\clang64\bin;C:\msys64\usr\bin;' + $env:PATH
$env:PYTHONPATH='C:\Lab\workspace\test.git\bindings\python;C:\Lab\workspace\test.git\benchmarks\python'
& 'C:\Lab\workspace\test.git\.venv-clang64\bin\python' `
  'C:\Lab\workspace\test.git\benchmarks\python\benchmark_wg04_pixel_decode.py' `
  'C:\Lab\img\WG04\IMAGES' `
  --backend dicomsdl --warmup 2 --repeat 10 `
  --json 'C:\Lab\workspace\test.git\build\wg04_dicomsdl_clang64_r10.json'
```

### 7.2 ucrt64

```powershell
$env:PATH='C:\msys64\ucrt64\bin;C:\msys64\usr\bin;' + $env:PATH
$env:PYTHONPATH='C:\Lab\workspace\test.git\bindings\python;C:\Lab\workspace\test.git\benchmarks\python'
& 'C:\Lab\workspace\test.git\.venv-ucrt64\bin\python' `
  'C:\Lab\workspace\test.git\benchmarks\python\benchmark_wg04_pixel_decode.py' `
  'C:\Lab\img\WG04\IMAGES' `
  --backend dicomsdl --warmup 2 --repeat 10 `
  --json 'C:\Lab\workspace\test.git\build\wg04_dicomsdl_ucrt64_r10.json'
```

### 7.3 MSVC (LTCG ON)

```powershell
$env:PYTHONPATH='C:\Lab\workspace\test.git\bindings\python;C:\Lab\workspace\test.git\benchmarks\python'
& 'C:\Lab\workspace\test.git\.venv-msvc\Scripts\python.exe' `
  'C:\Lab\workspace\test.git\benchmarks\python\benchmark_wg04_pixel_decode.py' `
  'C:\Lab\img\WG04\IMAGES' `
  --backend dicomsdl --warmup 2 --repeat 10 `
  --json 'C:\Lab\workspace\test.git\build\wg04_dicomsdl_msvc2026_r10.json'
```

### 7.4 clang-cl

```powershell
$env:PYTHONPATH='C:\Lab\workspace\test.git\bindings\python;C:\Lab\workspace\test.git\benchmarks\python'
& 'C:\Lab\workspace\test.git\.venv-clangcl\Scripts\python.exe' `
  'C:\Lab\workspace\test.git\benchmarks\python\benchmark_wg04_pixel_decode.py' `
  'C:\Lab\img\WG04\IMAGES' `
  --backend dicomsdl --warmup 2 --repeat 10 `
  --json 'C:\Lab\workspace\test.git\build\wg04_dicomsdl_clangcl2026_r10.json'
```

### 7.5 MSVC (LTCG OFF)

```powershell
$env:PYTHONPATH='C:\Lab\workspace\test.git\bindings\python;C:\Lab\workspace\test.git\benchmarks\python'
& 'C:\Lab\workspace\test.git\.venv-msvc-ltcgoff\Scripts\python.exe' `
  'C:\Lab\workspace\test.git\benchmarks\python\benchmark_wg04_pixel_decode.py' `
  'C:\Lab\img\WG04\IMAGES' `
  --backend dicomsdl --warmup 2 --repeat 10 `
  --json 'C:\Lab\workspace\test.git\build\wg04_dicomsdl_msvc2026_ltcgoff_r10.json'
```

## 8) Latest Benchmark Results (r10)

Most recent run settings:

- Date: `2026-03-09`
- Backend: DicomSDL
- WG04 root: `C:\Lab\img\WG04\IMAGES`
- Warmup / Repeat: `2 / 10`
- TOTAL row rule: HTJ2K rows excluded by script design

Summary (`TOTAL`, lower `ms/decode` is better):

| Rank | Toolchain | ms/decode | MPix/s | MiB/s | JSON |
|---|---|---:|---:|---:|---|
| 1 | `clang-cl-2026` | 14.473 | 236.867 | 360.748 | `build\wg04_dicomsdl_clangcl2026_r10.json` |
| 2 | `msys2-clang64` | 15.121 | 226.720 | 345.295 | `build\wg04_dicomsdl_clang64_r10.json` |
| 3 | `msys2-ucrt64` | 15.127 | 226.633 | 345.163 | `build\wg04_dicomsdl_ucrt64_r10.json` |
| 4 | `msvc-2026 (LTCG OFF)` | 16.159 | 212.159 | 323.118 | `build\wg04_dicomsdl_msvc2026_ltcgoff_r10.json` |
| 5 | `msvc-2026 (LTCG ON)` | 16.267 | 210.755 | 320.980 | `build\wg04_dicomsdl_msvc2026_r10.json` |

Key deltas from this run:

- `clang-cl` vs `msvc (LTCG ON)`: about `1.124x` faster
- `msys2 clang64` vs `msvc (LTCG ON)`: about `1.076x` faster
- `msys2 ucrt64` vs `msvc (LTCG ON)`: about `1.075x` faster
- `msvc LTCG OFF` vs `msvc LTCG ON`: about `1.007x` faster (`-0.664%` ms/decode)

## 9) Compare TOTAL Rows from JSON

```powershell
$files = @(
  @{name='clang-cl-2026'; path='C:\Lab\workspace\test.git\build\wg04_dicomsdl_clangcl2026_r10.json'},
  @{name='msys2-clang64'; path='C:\Lab\workspace\test.git\build\wg04_dicomsdl_clang64_r10.json'},
  @{name='msys2-ucrt64'; path='C:\Lab\workspace\test.git\build\wg04_dicomsdl_ucrt64_r10.json'},
  @{name='msvc-2026 (LTCG ON)'; path='C:\Lab\workspace\test.git\build\wg04_dicomsdl_msvc2026_r10.json'},
  @{name='msvc-2026 (LTCG OFF)'; path='C:\Lab\workspace\test.git\build\wg04_dicomsdl_msvc2026_ltcgoff_r10.json'}
)

$results = foreach ($f in $files) {
  $j = Get-Content -Raw $f.path | ConvertFrom-Json
  $rows = @($j.results.dicomsdl | Where-Object { $_.codec -notlike 'htj2k*' })
  $d = ($rows | Measure-Object decodes -Sum).Sum
  $e = ($rows | Measure-Object elapsed_s -Sum).Sum
  $px = ($rows | Measure-Object total_pixels -Sum).Sum
  $by = ($rows | Measure-Object total_bytes -Sum).Sum
  [PSCustomObject]@{
    env = $f.name
    ms_per_decode = [math]::Round($e * 1000.0 / $d, 3)
    mpix_s = [math]::Round($px / 1000000.0 / $e, 3)
    mib_s = [math]::Round($by / 1048576.0 / $e, 3)
  }
}

$results | Sort-Object ms_per_decode | Format-Table -AutoSize
```

## 10) Troubleshooting

- `ValueError: list.remove(x): x not in list`
  - Ensure `PYTHONPATH` is exactly set as shown before running benchmark.
- `Could not open file for write ... gitinfo.txt.tmp` during MSYS2 wheel build
  - Enable Windows long paths and reboot.
- `externally-managed-environment` when installing with MSYS2 Python
  - Use a virtual environment (`python -m venv ...`) and install inside it.
