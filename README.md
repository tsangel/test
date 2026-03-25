# dicomsdl

Minimal DICOM file wrapper with a C++ core and optional Python bindings.

Documentation language entry points:
[English](README.md) | [Korean](README.ko.md) | [Japanese](README.ja.md) | [Simplified Chinese](README.zh-CN.md)

English is the source of truth for repository docs. The Sphinx docs live under
`docs/en`, `docs/ko`, `docs/ja`, and `docs/zh-cn`; see
[docs/en/developer/translation_workflow.md](docs/en/developer/translation_workflow.md).

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

## Python Quickstart

```python
import dicomsdl as dicom

df = dicom.read_file("sample.dcm")
print(df["PatientName"].value)
print(df["Rows"].value, df["Columns"].value)

arr = df.to_array()
print(arr.shape, arr.dtype)
```

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

## Command-Line Tools

After `pip install dicomsdl`, these console scripts are available:

- `dicomdump`
- `dicomshow`
- `dicomconv`

See [CLI Tools](docs/en/guide/cli_tools.md) for usage and options.

## Documentation

Start here:

- [Docs home](docs/en/index.md)
- [Quickstart](docs/en/guide/quickstart.md)
- [Installation](docs/en/guide/installation.md)
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
