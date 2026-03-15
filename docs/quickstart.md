# Quickstart

## Python (recommended)
1. Requirements: Python 3.9+, `pip`, `cmake`.
2. Build/install

```bash
python -m pip install --upgrade pip cmake
pip wheel . --no-build-isolation --no-deps -w dist
pip install --force-reinstall dist/dicomsdl-*.whl
```

On macOS, if `MACOSX_DEPLOYMENT_TARGET` is unset, wheel builds default to
`10.15` for `x86_64` and `11.0` for `arm64`/`universal2`. Export
`MACOSX_DEPLOYMENT_TARGET=...` first if you need a different minimum target.

2.1 Quick wheel wrapper scripts (optional, fast packaging only)

```bash
# macOS / Linux: static wheel
./build-wheel-static.sh

# macOS / Linux: shared-plugin wheel
./build-wheel-shared.sh
```

```cmd
:: Windows (cmd.exe): static wheel
build-wheel-static.bat

:: Windows (cmd.exe): shared-plugin wheel
build-wheel-shared.bat
```

Wrapper defaults:

- Remove old outputs at start (`BUILD_DIR`, `build/temp.*`, `build/lib.*`, `build/bdist.*`, `WHEEL_DIR`).
- Force wheel build to Release (`FORCE_WHEEL_RELEASE=1`, `BUILD_TYPE=Release`).
- On macOS, default `MACOSX_DEPLOYMENT_TARGET` to `10.15` for `x86_64` and `11.0`
  for `arm64`/`universal2` when the variable is unset.
- Build a fresh wheel quickly without regression tests (`BUILD_TESTING=OFF`, `RUN_TESTS=0`).

To disable defaults:

- `STATIC_PRE_CLEAN_OUTPUTS=0` disables pre-clean.
- `FORCE_WHEEL_RELEASE=0` allows non-Release wheel builds.

2.2 Wheel wrapper with tests

```bash
# static profile (default)
./build-wheel-with-tests.sh

# shared profile
DICOMSDL_WHEEL_PROFILE=shared ./build-wheel-with-tests.sh
```

This path enables CTest and then runs `pytest -q tests/python` after building the wheel.

Useful build-script toggles:

- `DICOMSDL_MSVC_ENABLE_LTCG=OFF` disables MSVC `/GL` + `/LTCG` for the wheel build.
- `DICOMSDL_MSVC_PGO=OFF|GEN|USE` controls MSVC PGO mode (`OFF` default).
- `DICOMSDL_MSVC_PGO_DIR=...` chooses where `.pgd/.pgc` profile data is stored.
- `PYTEST_ARGS="..."` forwards extra arguments to `pytest`.

Then install the built wheel manually:

```bash
pip install --force-reinstall --no-deps --no-cache-dir dist-static-with-tests/dicomsdl-*.whl
# or: dist-shared-with-tests/dicomsdl-*.whl
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
- Recreate the decode plan and reallocate decode output buffers before calling
  `decode_into` again.
- Do not keep using an old output array shape/stride after metadata changes.
- `decode_into` raises if the requested frame, output buffer size/layout, or
  native decode backend state is invalid.

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

  // Need presence distinction, not just a default fallback? Use:
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
  ok &= ds.add_dataelement("Rows"_tag, dicom::VR::US).from_long(512);
  ok &= ds.add_dataelement("Columns"_tag, dicom::VR::US).from_long(-1); // failure example
  ok &= ds.add_dataelement("SOPInstanceUID"_tag, dicom::VR::UI)
            .from_uid_string("1.2.840.10008.5.1.4.1.1.2");

  if (!ok) {
    for (const auto& msg : reporter->take_messages()) {
      std::cerr << msg << '\n';
    }
  }
  dicom::diag::set_thread_reporter(nullptr);
}
```

- Full runnable example: `examples/batch_assign_with_error_check.cpp`
- `add_dataelement(...)` returns `DataElement&`, so write helpers chain with `.`.

## Quick build/test commands
- Python tests: build `_dicomsdl`, install `tests/python/requirements.txt`, then run `py -3.14 -m pytest tests/python -q`
- CTest: `cmake -S . -B build && cmake --build build && ctest --test-dir build`

## Related docs
- UID generation and append guide: [Generating UID](generating_uid.md)
