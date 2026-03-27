# DicomSDL

Fast and lightweight DICOM software development library with a C++ core and optional Python bindings.

## Introduction

DicomSDL is built for local, file-oriented DICOM workflows: reading datasets,
extracting metadata, decoding pixels, modifying values, and writing or
transcoding outputs. It is designed for high-throughput processing of large
collections of DICOM files, where startup cost, metadata access speed, and
pixel pipeline control matter.

It is a good fit for jobs such as metadata scanning, conversion pipelines,
dataset cleanup, machine-learning preprocessing, and image export. It is not a
DICOM network stack, so send/receive workflows over DICOM networking are out of
scope.

In practice, DicomSDL focuses on:

- fast DICOM file and in-memory buffer access
- typed metadata lookup and update in C++ and Python
- pixel decode, encode, and transfer-syntax transcode workflows
- caller-controlled buffer and layout paths for performance-sensitive code

## Install

For most Python users:

```bash
python -m pip install --upgrade pip
pip install "dicomsdl[numpy,pil]"
```

For a source checkout:

```bash
cmake -S . -B build -DDICOM_BUILD_EXAMPLES=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Need more than the default path?

- [Installation](docs/en/guide/installation.md)
- [Build C++ From Source](docs/en/developer/cpp_build_from_source.md)
- [Build Python From Source](docs/en/developer/build_python_from_source.md)

CI regularly builds and checks DicomSDL on:

- Linux x86_64 (`ubuntu-latest`, `manylinux` and `musllinux`), Python 3.9-3.12
- Linux aarch64 (`ubuntu-24.04-arm`, `manylinux` and `musllinux`), Python 3.9-3.12
- macOS x86_64 (`macos-15-intel`), Python 3.9-3.12
- macOS arm64 (`macos-14`), Python 3.9-3.12
- Windows AMD64 (`windows-latest`), Python 3.9-3.12

Additional CI jobs cover Ubuntu stub checks and Windows toolchain builds with
MSVC and MSYS2 `clang64`.

## Python Quickstart

```pycon
>>> import dicomsdl as dicom
>>> df = dicom.read_file("sample.dcm")
>>> df.PatientName
PersonName(...)
>>> df.Rows, df.Columns
(512, 512)
>>> df.StudyDate
'20040826'
>>> df["StudyDate"].value
'20040826'
>>> df.get_value("StudyDate")
'20040826'
>>> df.InstitutionAddress
None
>>> arr = df.to_array()
>>> arr.shape, arr.dtype
((512, 512), dtype('int16'))
```

For ordinary top-level standard DICOM keywords, `df.Rows` and
`df.PatientName` are usually the shortest reads in Python. Use
`df.get_value("Seq.0.Tag")` for nested lookups, and use `df["Rows"]` or
`df.get_dataelement(...)` when you need `DataElement` metadata instead of just
the typed value. Need decoded output layout details? `df.create_decode_plan()`
is the current replacement for older `getPixelDataInfo()`-style examples.
For more walkthrough-style examples, see the notebooks under
[`tutorials/`](tutorials/README.md).

## C++ Quickstart

```cpp
#include <dicom.h>
#include <iostream>
using namespace dicom::literals;

int main() {
  auto file = dicom::read_file("sample.dcm");
  auto& ds = file->dataset();

  long rows = ds["Rows"_tag].to_long().value_or(0);
  long cols = ds["Columns"_tag].to_long().value_or(0);
  std::cout << rows << " x " << cols << '\n';
}
```

## Documentation

Start here:

- [Docs home](docs/en/index.md)
- [Quickstart](docs/en/guide/quickstart.md)
- [Installation](docs/en/guide/installation.md)
- [Tutorial notebooks](tutorials/README.md)
- [CLI Tools](docs/en/guide/cli_tools.md)
- [Python DataSet Guide](docs/en/guide/python_dataset_guide.md)
- [C++ DataSet Guide](docs/en/guide/cpp_dataset_guide.md)
- [Reference index](docs/en/reference/index.md)
- [Developer index](docs/en/developer/index.md)

Build the docs locally:

```bash
python -m pip install -r docs/requirements.txt
./build-docs.sh html-all
```

## Notes

- Third-party dependencies are fetched by CMake `FetchContent` during configure.
- Example binaries such as `dicomdump` and `dicomconv` are built when `DICOM_BUILD_EXAMPLES=ON`.
- For pixel decode and encode safety details, see [Pixel Decode](docs/en/guide/pixel_decode.md) and [Pixel Encode](docs/en/guide/pixel_encode.md).
