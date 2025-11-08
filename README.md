# dicomsdl

Minimal DICOM file wrapper with optional Python bindings.

## Repository Setup

```
git submodule update --init --recursive
```

## C++ Build

```
cmake -S . -B build -DDICOM_BUILD_EXAMPLES=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Run C++ example

```
./build/dicom_example
```

## Python Wheel

Python ≥ 3.8 is required.

```
python -m pip install --upgrade pip
pip install cmake
pip wheel . --no-build-isolation --no-deps -w dist
```

After building, install the wheel:

```
pip install dist/dicomsdl-*.whl
```

### Run Python example

```
python examples/python/dicom_example.py
```

## Continuous Integration

GitHub Actions builds Python wheels for Linux, macOS (x86_64 & arm64), and Windows
across CPython 3.8 through 3.14. Generated wheels are published as workflow
artifacts.
