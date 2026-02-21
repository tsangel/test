# dicomsdl

Minimal DICOM file wrapper with optional Python bindings.

## Repository Setup

```
git submodule update --init --recursive
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
```

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

### Run Python examples

```
python examples/python/keyword_lookup_example.py PatientName Rows
python examples/python/tag_lookup_example.py 00100010 (0008,0016)
python examples/python/uid_lookup_example.py ExplicitVRLittleEndian 1.2.840.10008.1.2.1
python examples/python/dump_dataset_example.py path/to/file.dcm
python examples/python/pixel_decode_safe_example.py path/to/file.dcm --frame 0
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
