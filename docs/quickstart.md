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

4.1 Raw value bytes without copy

```python
elem = df.dataset.get_dataelement("PixelData")
if elem:
    raw = elem.value_span()  # memoryview
    print(raw.nbytes, list(raw[:8]))
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
#include <diagnostics.h>
#include <memory>
using namespace dicom::literals;

int main() {
  auto file = dicom::read_file("sample.dcm");
  auto& ds = file->dataset();

  long rows = ds["Rows"_tag].to_long().value_or(0);
  // process rows

  // Need missing/present distinction? Use:
  // if (auto& e = ds["Rows"_tag]; e) { ... }
}
```

3. Batch set with `ok &= ...` and error check
```cpp
#include <dicom.h>
#include <diagnostics.h>
#include <memory>
#include <iostream>
using namespace dicom::literals;

int main() {
  dicom::DataSet ds;
  auto reporter = std::make_shared<dicom::diag::BufferingReporter>(256);
  dicom::diag::set_thread_reporter(reporter);

  bool ok = true;
  ok &= ds.add_dataelement("Rows"_tag, dicom::VR::US)->from_long(512);
  ok &= ds.add_dataelement("Columns"_tag, dicom::VR::US)->from_long(-1); // failure example
  ok &= ds.add_dataelement("SOPInstanceUID"_tag, dicom::VR::UI)
            ->from_uid_string("1.2.840.10008.5.1.4.1.1.2");

  if (!ok) {
    for (const auto& msg : reporter->take_messages()) {
      std::cerr << msg << '\n';
    }
  }
  dicom::diag::set_thread_reporter(nullptr);
}
```

- Full runnable example: `examples/batch_assign_with_error_check.cpp`

## Quick build/test commands
- Python tests: `pytest -q tests/python`
- CTest: `cmake -S . -B build && cmake --build build && ctest --test-dir build`

## Related docs
- UID generation and append guide: [Generating UID](generating_uid.md)
