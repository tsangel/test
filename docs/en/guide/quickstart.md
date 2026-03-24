# Quickstart

## Python
For most users, start with the PyPI install path.
1. Requirements: Python 3.9+, `pip`.
2. Install from PyPI

```bash
python -m pip install --upgrade pip
pip install "dicomsdl[numpy,pil]"
```

If `pip` falls back to building from source on your platform, install `cmake` first.

```{note}
If you only need metadata access, file IO, or transcode workflows on a server,
`pip install dicomsdl` is enough.
```

Need a source build, custom wheel, or test workflow? See [Build Python From Source](../developer/build_python_from_source.md).
Need platform-specific install details? See [Installation](installation.md).

3. Read metadata

```pycon
>>> import dicomsdl as dicom
>>> df = dicom.read_file("sample.dcm")
>>> df["PatientName"].value
PersonName(Doe^Jane)
>>> df["Rows"].value, df["Columns"].value
(512, 512)
```

`DicomFile` forwards the root `DataSet` access helpers, so `df["Rows"]`,
`df.get_value(...)`, and `df.get_dataelement(...)` work directly in Python.
Use `df.dataset` when you want the dataset boundary to be explicit.
`PatientName` is a `PN`, so `.value` prints as a `PersonName(...)` object rather
than a plain Python string.
Need the object model, metadata lookup rules, or the full decode flow? See
[Core Objects](core_objects.md), [Reading Data Element Values](reading_data.md), and
[Pixel Decode](pixel_decode.md).

4. Decode pixels into a NumPy array

```pycon
>>> import dicomsdl as dicom
>>> df = dicom.read_file("sample.dcm")
>>> arr = df.to_array()
>>> arr.shape
(512, 512)
>>> arr.dtype
dtype('uint16')
```

Need decode options, frame selection, or output layout control? See
[Pixel Decode](pixel_decode.md).

5. Quick image preview with Pillow

```bash
pip install "dicomsdl[numpy,pil]"
```

```python
import dicomsdl as dicom

df = dicom.read_file("sample.dcm")
image = df.to_pil_image(frame=0)
image.show()
```

`to_pil_image()` is a narrow convenience helper for quick visual inspection.
Prefer `to_array()` for analysis pipelines and repeatable processing. `show()`
depends on your local GUI/viewer and may not work in headless environments.
Need decode options or array-oriented workflows instead? See
[Pixel Decode](pixel_decode.md).

6. Transcode to `HTJ2KLossless` and write a new file

```python
from pathlib import Path

import dicomsdl as dicom

in_path = Path("in.dcm")
out_path = Path("out_htj2k_lossless.dcm")

df = dicom.read_file(in_path)
df.set_transfer_syntax("HTJ2KLossless")
df.write_file(out_path)

print("Input bytes:", in_path.stat().st_size)
print("Output bytes:", out_path.stat().st_size)
```

For one representative file, the script prints something like:

```text
Input bytes: 525312
Output bytes: 287104
```

This file-to-file transcode path also works with the base `pip install dicomsdl`
install. The exact size change depends on the source transfer syntax, pixel
content, and metadata.
Need lossy encode options, codec limits, or streaming write guidance? See
[Pixel Encode](pixel_encode.md), [Pixel Encode Constraints](../reference/pixel_encode_constraints.md),
and [Encode-capable Transfer Syntax Families](../reference/codec_support_matrix.md).

7. Access `DataElement` value bytes through a `memoryview`

```python
import dicomsdl as dicom

df = dicom.read_file("sample.dcm")
elem = df["PixelData"]
if elem:
    raw = elem.value_span()  # memoryview
    print("Raw bytes:", raw.nbytes)
    print("Head:", list(raw[:8]))
```

For an uncompressed `512 x 512` `uint16` image:

```text
Raw bytes: 524288
Head: [34, 12, 40, 12, 36, 12, 39, 12]
```

The exact first bytes depend on the file. This direct `value_span()` view is for
native / uncompressed `PixelData`. For compressed encapsulated transfer syntaxes,
`PixelData` is stored as a `PixelSequence`, so `elem.value_span()` is empty and
you should use `elem.pixel_sequence.frame_encoded_memoryview(0)` or
`elem.pixel_sequence.frame_encoded_bytes(0)` instead.
Keep `df` alive while you use `raw`: the memoryview points at bytes owned by the
loaded DICOM object and becomes invalid if those bytes are replaced.
Need raw-byte semantics or encapsulated `PixelData` details? See
[DataElement Reference](../reference/dataelement_reference.md) and
[Pixel Reference](../reference/pixel_reference.md).

Need the full decode safety model? See [Pixel Decode](pixel_decode.md) and
[Error Handling](error_handling.md).

## C++
Build from a repository checkout.
Requirements: `git`, `CMake`, `C++20` compiler.
1. Clone the repository

```bash
git clone https://github.com/tsangel/dicomsdl.git
cd dicomsdl
```

2. Configure & build
```bash
cmake -S . -B build -DDICOM_BUILD_EXAMPLES=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

3. Usage snippet
```cpp
#include <dicom.h>
#include <diagnostics.h>
#include <iostream>
#include <memory>
using namespace dicom::literals;

int main() {
  auto file = dicom::read_file("sample.dcm");
  auto& ds = file->dataset();

  long rows = ds["Rows"_tag].to_long().value_or(0);
  // When presence itself matters, branch on the element before decoding it.
  long cols = 0;
  if (auto& e = ds["Columns"_tag]; e) {
    cols = e.to_long().value_or(0);
  }
  std::cout << "Image size: " << rows << " x " << cols << '\n';
}
```

Typical output looks like this:

```text
Image size: 512 x 512
```

Need more C++ API detail? See [C++ API Overview](../reference/cpp_api.md) and
[DataSet Reference](../reference/dataset_reference.md).

4. Batch set with `ok &= ...` and error check
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

  if (!ok) {
    for (const auto& msg : reporter->take_messages()) {
      std::cerr << msg << '\n';
    }
  }
  dicom::diag::set_thread_reporter(nullptr);
}
```

With the intentional `Columns = -1` failure above, the output looks like this.
`VR::US` only accepts unsigned values, so `Columns = -1` triggers a range error:

```text
[ERROR] from_long tag=(0028,0011) vr=US reason=value out of range for VR
```

- Full runnable example: `examples/batch_assign_with_error_check.cpp`
- `add_dataelement(...)` returns `DataElement&`, so write helpers chain with `.`.
Need broader write patterns or failure-handling guidance? See
[Writing Data Element Values](writing_data.md) and [Error Handling](error_handling.md).
