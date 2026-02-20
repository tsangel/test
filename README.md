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
```

Python 코드에서는 일관되게 `import dicomsdl as dicom` 형식의 alias를 사용합니다.

### Run Python Tests

```bash
python -m venv .venv && source .venv/bin/activate
python -m pip install --upgrade pip cmake pytest
pip install -e .
pytest -q tests/python
```

Windows에서는 PowerShell/`cmd`에서 가상환경 활성화만 플랫폼에 맞게 바꿔주면 동일합니다.

## Python Wheel Quick Commands

### macOS / Linux

```bash
python -m venv .venv && source .venv/bin/activate
python -m pip install --upgrade pip cmake
pip wheel . --no-build-isolation --no-deps -w dist
pip install --force-reinstall dist/dicomsdl-*.whl
python -c "import dicomsdl as dicom; tag, vr = dicom.keyword_to_tag_vr('PatientName'); print(int(tag), vr.str())"
python -c "import dicomsdl as dicom; ds = dicom.read_file('sample.dcm'); print(ds.path)"
python -c "import dicomsdl as dicom; data = b'DICM'; ds = dicom.read_bytes(data, name='inline'); print(ds.path, len(data))"
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
