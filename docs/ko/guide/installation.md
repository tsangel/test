# 설치

반복 가능한 설치 / 빌드 경로가 필요하거나, dicomsdl이 현재 어떤 환경을 지원하는지 더 분명하게 보고 싶다면 이 페이지를 사용하세요.

## 환경 요약

### Python / PyPI

- CPython `3.9`에서 `3.14`
- 현재 wheel CI 대상:
  - Linux `x86_64`
  - macOS `x86_64`
  - macOS `arm64`
  - Windows `AMD64`
- 현재 `musllinux` wheel은 빌드하지 않음
- base install만으로 metadata access, file I/O, transcode workflow 사용 가능
- NumPy 기반 pixel helper와 Pillow preview용 optional extra 제공

### C++ / source build

- `git`
- `CMake 3.16+`
- `C++20` 컴파일러
- `Ninja`를 권장하지만, 일부 Windows toolchain에서만 필수
- Python은 Python binding이나 wheel을 빌드할 때만 필요

### Windows toolchain

`build.bat`는 다음 source-build 경로를 문서화하고 지원합니다.

- `MSVC` (`cl.exe`)
- `clang-cl` + `Ninja`
- `MSYS2 clang64` + `Ninja`

`DICOMSDL_WINDOWS_TOOLCHAIN`을 설정하지 않으면 `build.bat`가 `PATH`에 있는 도구를 기준으로 `msvc`, `clangcl`, `clang64` 순서로 자동 선택합니다.

### macOS deployment target

- `x86_64` 빌드 기본값은 macOS `10.15`
- `arm64` 빌드 기본값은 macOS `11.0`
- 명시적인 제어가 필요할 때만 `MACOSX_DEPLOYMENT_TARGET`을 설정하세요

## 경로 선택

- PyPI install: Python binding을 가장 빨리 써보는 방법
- checkout에서 C++ build: dicomsdl을 자신의 C++ application에 통합할 때 가장 적합한 경로
- Unix 계열 `build.sh`: macOS / Linux 빌드를 위한 convenience wrapper
- checkout에서 Python source build: [Build Python From Source](../developer/build_python_from_source.md)에 문서화
- Windows `build.bat`: 명시적인 toolchain 선택이 필요할 때 권장되는 진입점

## PyPI 설치

```bash
python -m pip install --upgrade pip
pip install dicomsdl
```

현재 플랫폼에서 `pip`가 source build로 되돌아가면 먼저 `cmake`를 설치하세요.

선택 가능한 Python extra:

- `pip install "dicomsdl[numpy]"`: `to_array(...)`, `decode_into(...)`, NumPy 기반 pixel helper
- `pip install "dicomsdl[numpy,pil]"`: `to_pil_image(...)` convenience helper까지 함께 사용하려는 경우 (`numpy` + `Pillow`)

서버에서 metadata access, file I/O, transcode workflow만 필요하다면 기본 `pip install dicomsdl` 경로로 충분합니다.

## checkout에서 C++ 빌드

```bash
git clone https://github.com/tsangel/dicomsdl.git
cd dicomsdl
cmake -S . -B build -DDICOM_BUILD_EXAMPLES=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Linux와 macOS에서는 이것이 표준 CMake 경로입니다. Windows에서는 이미 특정 generator / toolchain을 고정해 둔 상태가 아니라면 `build.bat`를 우선하세요.

## Unix 계열 `build.sh`

macOS와 Linux에서 사용하는 wrapper script는 다음과 같습니다.

```bash
./build.sh
```

특정 target만 빌드하려면:

```bash
./build.sh dicomsdl
./build.sh dicomsdl _dicomsdl
```

뒤에 붙는 인자는 `cmake --build --target ...` target으로 전달됩니다.

## Windows `build.bat`

가장 짧은 Windows 진입점은 다음과 같습니다.

```bat
call build.bat dicomsdl
```

특정 toolchain을 강제하려면:

```bat
set DICOMSDL_WINDOWS_TOOLCHAIN=msvc
call build.bat dicomsdl
```

필요에 따라 `msvc` 대신 `clangcl` 또는 `clang64`를 사용하세요.

`MSVC`와 `clang-cl`은 초기화된 Visual Studio developer environment에서 사용하세요. `clang64`는 `clang`, `clang++`, `cmake`, `ninja`가 `PATH`에 있는 MSYS2 `clang64` 환경에서 실행해야 합니다.

`build.sh`와 마찬가지로 뒤에 붙는 인자는 `cmake --build --target ...` target으로 전달됩니다.

## 공통 build script 옵션

`build.sh`와 `build.bat`는 같은 핵심 환경 변수를 공통으로 사용합니다.

- `BUILD_DIR`: build directory 경로
- `BUILD_TYPE`: `Release`, `Debug` 같은 build configuration
- `BUILD_TESTING=ON|OFF`: CTest target 활성화 또는 비활성화
- `DICOM_BUILD_EXAMPLES=ON|OFF`: example binary 빌드 여부
- `RUN_TESTS=1|0`: build 후 CTest 실행 여부
- `BUILD_WHEEL=1|0`: Python wheel 빌드 여부
- `WHEEL_ONLY=1`: top-level CMake configure/build를 건너뛰고 wheel만 빌드
- `WHEEL_DIR=...`: 생성된 wheel의 출력 디렉터리
- `PYTHON_BIN=...`: wheel build에 사용할 Python interpreter
- `CMAKE_EXTRA_ARGS="..."`: raw CMake configure argument 추가
- `BUILD_PARALLELISM=N`: 자동 감지된 병렬 빌드 수를 덮어씀
- `RESET_CMAKE_CACHE=1`: generator/compiler 설정이 바뀌었을 때 오래된 CMake cache 정리
- `DICOMSDL_PIXEL_DEFAULT_MODE=builtin|shared|none`: 기본 codec plugin 모드
- `DICOMSDL_PIXEL_<CODEC>_MODE=builtin|shared|none`: `JPEG`, `JPEGLS`, `JPEG2K`, `HTJ2K`, `JPEGXL`별 override

예시:

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

## OS별 옵션

### macOS / `build.sh`

- `MACOSX_DEPLOYMENT_TARGET=...`: 기본 deployment target 덮어쓰기
- `CMAKE_OSX_ARCHITECTURES=...`: target architecture를 명시적으로 선택
- `ARCHFLAGS=...`: `CMAKE_OSX_ARCHITECTURES`가 unset일 때 wheel build architecture 감지에 영향

### Linux / `build.sh`

- `CMAKE_GENERATOR=...`: script의 자동 선택 대신 generator를 강제
- `BUILD_PARALLELISM=N`: CPU 사용량을 명시적으로 제한하고 싶을 때 유용

### Windows / `build.bat`

- `DICOMSDL_WINDOWS_TOOLCHAIN=auto|msvc|clangcl|clang64`: Windows toolchain 선택
- `CMAKE_GENERATOR=...`: script가 선택한 generator를 덮어씀
- `DICOMSDL_MSVC_ENABLE_LTCG=ON|OFF`: MSVC link-time code generation 토글
- `DICOMSDL_MSVC_PGO=OFF|GEN|USE`: MSVC PGO 모드 제어
- `DICOMSDL_MSVC_PGO_DIR=...`: `.pgd/.pgc` profile data 디렉터리
- `FORCE_WHEEL_RELEASE=1|0`: wheel build를 `Release`로 고정하거나 non-Release build 허용
- `VALIDATE_WHEEL_RUNTIME=1|0`: Windows wheel runtime validation 실행 또는 생략

## Python source build

로컬 wheel, editable에 가까운 개발 흐름, custom Python build 환경이 필요하면 [Build Python From Source](../developer/build_python_from_source.md)를 사용하세요.

## 관련 문서

- [Quickstart](quickstart.md)
- [Python DataSet Guide](python_dataset_guide.md)
- [Build Python From Source](../developer/build_python_from_source.md)
