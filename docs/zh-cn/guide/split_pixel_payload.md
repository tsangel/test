# Split Pixel Payload

Split Pixel Payload is a DicomSDL runtime convention for keeping DICOM metadata
and large Pixel Data bytes in separate buffers. It is useful for viewers,
series packers, encryption pipelines, and cache layers where metadata must stay
available for a long time, but compressed or native pixel bytes are only needed
while decoding frames.

This is not a DICOM interchange format. It is a private DicomSDL convention for
your own storage/runtime pipeline. If you need a standards-compliant DICOM file,
write a normal DICOM with `write_bytes()` or `write_with_transfer_syntax()`.

## Model

A split instance has two byte buffers:

- **main P10 DICOM**
  - A regular Part 10 DICOM byte stream, except root `(7FE0,0010) PixelData`
    stores only a fixed 4-byte placeholder.
  - The placeholder value is exactly `DXP1`.
  - The placeholder is written as `OB`.
- **PixelData payload**
  - The complete PixelData value bytes that were removed from the main DICOM.
  - For native/uncompressed transfer syntaxes, this is the raw native PixelData
    value.
  - For encapsulated/compressed transfer syntaxes, this is the full
    encapsulated PixelData value field: Basic Offset Table item, fragment
    items, and sequence delimiter.

When the two buffers are attached together, metadata is read from the main P10
DICOM and Pixel Data is read from the payload buffer.

## When To Use It

Use split payloads when:

- a volume viewer needs DICOM metadata throughout the viewer lifetime;
- encoded Pixel Data can be released after selected frames are decoded;
- a series pack format stores metadata and payload in separate records;
- the main DICOM and pixel payload need separate encryption/compression;
- you want to avoid DICOM JSON just to keep metadata without large pixel bytes.

Do not use split payloads when:

- the output is meant to be exchanged as a standard DICOM file;
- downstream tools do not know DicomSDL's `DXP1` convention;
- you need `FloatPixelData` or `DoubleFloatPixelData` splitting.

Version 1 supports root `(7FE0,0010) PixelData` only.

## Write Split Buffers

### Python: Split Without Transcoding

```python
from pathlib import Path
import dicomsdl as dicom

source = dicom.read_file("image.dcm")

main_bytes, pixel_payload = source.write_bytes_split_pixel_payload()

Path("image.main.dcm").write_bytes(main_bytes)
Path("image.pixel_payload.bin").write_bytes(pixel_payload)
```

`main_bytes` contains the `DXP1` placeholder. `pixel_payload` contains the
removed PixelData value bytes.

### Python: Split While Writing A Target Transfer Syntax

```python
from pathlib import Path
import dicomsdl as dicom

source = dicom.read_file("image.dcm")

main_bytes, pixel_payload = source.write_with_transfer_syntax_split_pixel_payload(
    "RLELossless"
)

Path("image-rle.main.dcm").write_bytes(main_bytes)
Path("image-rle.pixel_payload.bin").write_bytes(pixel_payload)
```

Use `write_with_transfer_syntax_split_pixel_payload()` when your pipeline may
transcode before storing the split buffers.

### C++: Split Without Transcoding

```cpp
#include <cstdint>
#include <dicom.h>
#include <fstream>
#include <vector>

void write_file(const char* path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
}

auto source = dicom::read_file("image.dcm");
auto split = source->write_bytes_split_pixel_payload();

write_file("image.main.dcm", split.dicom_bytes);
write_file("image.pixel_payload.bin", split.pixel_payload_bytes);
```

### C++: Split While Writing A Target Transfer Syntax

```cpp
#include <dicom.h>

using namespace dicom::literals;

auto source = dicom::read_file("image.dcm");
auto split = source->write_with_transfer_syntax_split_pixel_payload(
    "RLELossless"_uid);
```

## Read Split Buffers

The PixelData payload memory is borrowed by DicomSDL. It must remain alive until
you call `detach_pixel_payload()` or destroy the `DicomFile`.

### Python

```python
from pathlib import Path
import dicomsdl as dicom

main_bytes = bytearray(Path("image.main.dcm").read_bytes())
pixel_payload = bytearray(Path("image.pixel_payload.bin").read_bytes())

df = dicom.read_bytes_with_pixel_payload(
    main_bytes,
    pixel_payload,
    name="image-split",
    copy=False,
)

print(df.Rows, df.Columns)
frame0 = df.pixel_data(0)

# Release DicomSDL's borrowed references before freeing or reusing the payload.
df.detach_pixel_payload()
pixel_payload.clear()
```

With `copy=False`, Python keeps internal owner references to `main_bytes` and
`pixel_payload` while the `DicomFile` is attached. Calling
`detach_pixel_payload()` removes the payload owner.

### C++

```cpp
#include <cstdint>
#include <dicom.h>
#include <vector>

std::vector<std::uint8_t> main_bytes = read_all_bytes("image.main.dcm");
std::vector<std::uint8_t> pixel_payload =
    read_all_bytes("image.pixel_payload.bin");

auto file = dicom::read_bytes_with_pixel_payload(
    "image-split",
    main_bytes.data(), main_bytes.size(),
    pixel_payload.data(), pixel_payload.size());

auto frame0 = file->pixel_data(0);

file->detach_pixel_payload();
pixel_payload.clear();
pixel_payload.shrink_to_fit();
```

In C++, the caller owns both vectors. Do not release or reallocate the payload
vector while the `DicomFile` still has an attached payload.

## Detach And Metadata Lifetime

After `detach_pixel_payload()`:

- metadata access still works;
- `has_attached_pixel_payload` / `has_attached_pixel_payload()` becomes false;
- pixel decode and encoded-frame access fail with a clear "payload is detached"
  error;
- the PixelData element keeps only a lightweight detached marker by default.

Use `keep_dump=True` in Python or `detach_pixel_payload(true)` in C++ if you
want to keep the current PixelData dump text for diagnostics.

```python
df.detach_pixel_payload(keep_dump=True)
print(df.dump())
```

```cpp
file->detach_pixel_payload(true);
std::cout << file->dump();
```

Keeping dump text can be useful for debugging frame and fragment indexing, but
it allocates a string that may walk all frame/fragment metadata. The default
detach path avoids that cost.

## Dump Output And Offsets

Dump output from a normal source DICOM and a reattached split DICOM may not be
byte-for-byte identical.

For metadata elements, the values should normally match. For PixelData, offsets
can differ:

- a normal DICOM reports offsets in the original file stream;
- a reattached encapsulated PixelData reports frame and fragment offsets relative
  to the external payload buffer;
- the main P10 placeholder itself is not used as the PixelSequence backing
  stream.

If you compare dumps, compare semantic fields and frame payload bytes rather
than raw offset text.

```python
source = dicom.read_file("image.dcm")
main_bytes, payload = source.write_bytes_split_pixel_payload()
roundtrip = dicom.read_bytes_with_pixel_payload(main_bytes, payload)

assert roundtrip["PixelData"].vr == source["PixelData"].vr
assert roundtrip.encoded_pixel_frame_bytes(0) == source.encoded_pixel_frame_bytes(0)
```

Some real-world encapsulated DICOM files contain odd fragment item lengths even
though DICOM item value lengths are expected to be even. DicomSDL accepts such
files when reading. When writing split payloads, DicomSDL serializes fragment
items with even value lengths and adds padding when needed.

## Validate The Placeholder

Reading split buffers requires a valid PixelData placeholder in the main DICOM.
DicomSDL rejects these cases:

- PixelData is missing from the main P10 DICOM;
- PixelData value length is not exactly 4 bytes;
- PixelData value is not `DXP1`;
- the external pixel payload pointer is null or the payload length is zero;
- encapsulated payload bytes cannot be indexed as a PixelSequence.

You can check the placeholder directly:

```python
main_only = dicom.read_bytes(main_bytes)
assert main_only["PixelData"].value_bytes() == dicom.PIXEL_PAYLOAD_PLACEHOLDER_MAGIC
```

## Example Programs

The repository includes matching C++ and Python examples:

```bash
cmake -S . -B build -DDICOM_BUILD_EXAMPLES=ON
cmake --build build --target split_pixel_payload_example
```

Read already split buffers:

```bash
build/_deps/dicomsdl_openjpeg-build/bin/split_pixel_payload_example \
  image.main.dcm image.pixel_payload.bin 0
```

Create split buffers:

```bash
build/_deps/dicomsdl_openjpeg-build/bin/split_pixel_payload_example \
  --split image.dcm image.main.dcm image.pixel_payload.bin 0
```

Python:

```bash
python examples/python/split_pixel_payload_example.py \
  --split image.dcm image.main.dcm image.pixel_payload.bin
```

```bash
python examples/python/split_pixel_payload_example.py \
  --split image.dcm image-rle.main.dcm image-rle.pixel_payload.bin \
  --transfer-syntax RLELossless
```

## API Summary

Python:

- `DicomFile.write_bytes_split_pixel_payload() -> (bytes, bytes)`
- `DicomFile.write_with_transfer_syntax_split_pixel_payload(...) -> (bytes, bytes)`
- `dicomsdl.read_bytes_with_pixel_payload(main, payload, name=..., copy=...)`
- `DicomFile.detach_pixel_payload(keep_dump=False)`
- `DicomFile.has_attached_pixel_payload`

C++:

- `DicomFile::write_bytes_split_pixel_payload(...)`
- `DicomFile::write_with_transfer_syntax_split_pixel_payload(...)`
- `read_bytes_with_pixel_payload(...)`
- `DicomFile::attach_to_memory_with_pixel_payload(...)`
- `DicomFile::detach_pixel_payload(bool keep_dump = false)`
- `DicomFile::has_attached_pixel_payload()`

