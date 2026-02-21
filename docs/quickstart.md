# Quickstart

## Python (recommended)
1. Requirements: Python 3.8+, `pip`, `cmake`.
2. Build/install

```bash
python -m pip install --upgrade pip cmake
pip wheel . --no-build-isolation --no-deps -w dist
pip install --force-reinstall dist/dicomsdl-*.whl
```

3. Five-line example

```python
import dicomsdl as dicom
# keyword -> (Tag, VR)
tag, vr = dicom.keyword_to_tag_vr("PatientName")
print(int(tag), vr.str())
```

4. Read/iterate a DataSet

```python
import dicomsdl as dicom

# Read a file into a DicomFile, then access root DataSet
df = dicom.read_file("sample.dcm")
for elem in df.dataset:
    print(elem.tag, elem.vr)
```

5. Pixel decode safety note

- If you change pixel-related tags or transfer syntax, treat previously derived
  pixel layout assumptions as stale.
- Re-query pixel metadata and reallocate decode output buffers before calling
  `decode_into` again.
- Do not keep using an old output array shape/stride after metadata changes.

## C++ (example build)
1. Configure & build
```bash
cmake -S . -B build -DDICOM_BUILD_EXAMPLES=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

2. Usage snippet
```cpp
#include <dicom.h>
using namespace dicom::literals;

int main() {
  constexpr dicom::Tag tag = "Rows"_tag;
  auto [kw_tag, vr] = dicom::keyword_to_tag_vr("PatientName");
  dicom::DataSet ds;
  ds.attach_to_file("sample.dcm");
  for (auto& elem : ds) {
    // process elements
  }
}
```

## Quick build/test commands
- Python tests: `pytest -q tests/python`
- CTest: `cmake -S . -B build && cmake --build build && ctest --test-dir build`

## Related docs
- UID generation and append guide: [Generating UID](generating_uid.md)
