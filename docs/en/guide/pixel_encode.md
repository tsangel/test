# Pixel Encode

Use `set_pixel_data()` when you already have native stored pixels to encode. Use `set_transfer_syntax()` when the current `DicomFile` already has pixel data and you want to transcode it in memory. Use `write_with_transfer_syntax()` when the goal is output in a different transfer syntax without mutating the source object first. Create an `EncoderContext` when the same transfer syntax and option set will be reused across multiple calls, or when you want to validate that configuration before starting a longer encode loop.

## Key Encoding APIs

**C++**

- `set_pixel_data(...)`
  - Replace Pixel Data from a native source buffer whose layout you describe explicitly with `pixel::ConstPixelSpan`.
- `set_pixel_data(..., frame_index)`
  - Encode one single-frame native source buffer into an existing encapsulated `PixelData` frame slot while keeping the surrounding multi-frame payload in place.
- `create_encoder_context(...)` + `set_pixel_data(...)` / `set_transfer_syntax(...)`
  - Keep one configured transfer syntax and option set outside a repeated encode or transcode loop.
- `write_with_transfer_syntax(...)`
  - Write a different transfer syntax directly to a file or stream without mutating the in-memory `DicomFile`.

**Python**

- `set_pixel_data(...)`
  - Replace Pixel Data from a C-contiguous NumPy array or other contiguous numeric buffer.
- `set_pixel_data(..., frame_index=...)`
  - Encode one single-frame array into an existing encapsulated `PixelData` frame slot.
- `create_encoder_context(...)` + `set_pixel_data(...)` / `set_transfer_syntax(...)`
  - Parse and validate one Python `options` object up front, then reuse the resulting context across repeated calls.
- `write_with_transfer_syntax(...)`
  - Write a different transfer syntax directly to a file without mutating the source object first.
- `set_transfer_syntax(...)`
  - Transcode the current `DicomFile` in memory when you want later reads or writes from the same object to use the new syntax.

## Relevant DICOM standard sections

- The pixel metadata that must stay consistent with encoded data is defined in [DICOM PS3.3 Section C.7.6.3, Image Pixel Module](https://dicom.nema.org/medical/dicom/current/output/chtml/part03/sect_C.7.6.3.html).
- Native versus encapsulated Pixel Data encoding and the codec-specific 8.2.x rules are defined in [DICOM PS3.5 Chapter 8, Encoding of Pixel, Overlay and Waveform Data](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/chapter_8.html) and [Section 8.2, Native or Encapsulated Format Encoding](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/sect_8.2.html).
- Encapsulated transfer syntax and fragment rules are defined in [DICOM PS3.5 Section A.4, Transfer Syntaxes for Encapsulation of Encoded Pixel Data](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/sect_A.4.html).
- In file-based encode and transcode workflows, the resulting Transfer Syntax UID is carried in the file meta information defined by [DICOM PS3.10 Chapter 7, DICOM File Format](https://dicom.nema.org/medical/dicom/current/output/chtml/part10/chapter_7.html).

## C++

### Describe source pixels explicitly before calling `set_pixel_data()`

```cpp
#include <cstdint>
#include <dicom.h>
#include <random>
#include <span>
#include <vector>

using namespace dicom::literals;

auto file = dicom::read_file("sample.dcm");

const std::uint32_t rows = 256;
const std::uint32_t cols = 256;
const std::uint32_t frames = 1;

std::vector<std::uint16_t> pixels(rows * cols * frames);
std::mt19937 rng(0);
std::uniform_int_distribution<int> dist(0, 4095);
for (auto& px : pixels) {
    px = static_cast<std::uint16_t>(dist(rng));
}

const dicom::pixel::ConstPixelSpan source{
    .layout = dicom::pixel::PixelLayout{
        .data_type = dicom::pixel::DataType::u16,
        .photometric = dicom::pixel::Photometric::monochrome2,
        .planar = dicom::pixel::Planar::interleaved,
        .reserved = 0,
        .rows = rows,
        .cols = cols,
        .frames = frames,
        .samples_per_pixel = 1,
        .bits_stored = 12,
        .row_stride = cols * sizeof(std::uint16_t),
        .frame_stride = rows * cols * sizeof(std::uint16_t),
    },
    .bytes = std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(pixels.data()),
        pixels.size() * sizeof(std::uint16_t)),
};

// set_pixel_data() uses the layout above to read the native source buffer and
// rewrites the matching image-pixel metadata on the DicomFile.
file->set_pixel_data("RLELossless"_uid, source);
```

### Keep one preconfigured context outside a repeated write loop

```cpp
#include <array>
#include <dicom.h>
#include <diagnostics.h>
#include <iostream>
#include <span>

using namespace dicom::literals;

auto file = dicom::read_file("sample.dcm");

const std::array<dicom::pixel::CodecOptionTextKv, 3> j2k_options{{
    {"target_psnr", "45"},
    {"threads", "4"},
    {"color_transform", "true"},
}};

// Build one reusable JPEG 2000 context outside the repeated write loop.
// This keeps the transfer syntax and option set in one place instead of
// rebuilding the same option list at each call site.
auto j2k_ctx = dicom::pixel::create_encoder_context(
    "JPEG2000"_uid,
    std::span<const dicom::pixel::CodecOptionTextKv>(j2k_options));

try {
    for (const char* path : {"out_j2k_1.dcm", "out_j2k_2.dcm"}) {
        file->write_with_transfer_syntax(path, "JPEG2000"_uid, j2k_ctx);
    }
} catch (const dicom::diag::DicomException& ex) {
    // When encode or configure fails, the exception message carries the
    // stage/reason detail for the failing call. Logging ex.what() is usually
    // enough for a first pass at debugging.
    std::cerr << ex.what() << '\n';
}
```

### Write a different transfer syntax directly to output

```cpp
#include <dicom.h>

using namespace dicom::literals;

auto file = dicom::read_file("sample.dcm");

// write_with_transfer_syntax() is the output-oriented transcode path when the
// target syntax matters only for the serialized result.
file->write_with_transfer_syntax("out_rle.dcm", "RLELossless"_uid);

// The same API family also has std::ostream variants in C++.
```

### Pass explicit codec options

```cpp
#include <array>
#include <dicom.h>

using namespace dicom::literals;

auto file = dicom::read_file("sample.dcm");

const std::array<dicom::pixel::CodecOptionTextKv, 1> lossy_options{{
    {"target_psnr", "45"},
}};

// For lossy targets, pass the codec options you want explicitly instead of
// relying on defaults that may not match your intended output. This direct
// style is fine for one-off writes; use EncoderContext instead when the same
// option set will be reused across many calls.
file->write_with_transfer_syntax(
    "out_j2k_lossy.dcm", "JPEG2000"_uid,
    std::span<const dicom::pixel::CodecOptionTextKv>(lossy_options));
```

## Python

### Replace Pixel Data from a NumPy array

```python
import dicomsdl as dicom
import numpy as np

df = dicom.read_file("sample.dcm")

# set_pixel_data() expects a C-contiguous numeric array.
# The array shape and dtype determine the encoded Rows, Columns,
# SamplesPerPixel, NumberOfFrames, and bit-depth metadata.
rng = np.random.default_rng(0)
arr = rng.integers(0, 4096, size=(256, 256), dtype=np.uint16)
df.set_pixel_data("ExplicitVRLittleEndian", arr)

df.write_file("native_replaced.dcm")
```

### Pass explicit codec options to `set_pixel_data()`

```python
import dicomsdl as dicom
import numpy as np

df = dicom.read_file("sample.dcm")
rng = np.random.default_rng(0)
arr = rng.integers(0, 4096, size=(256, 256), dtype=np.uint16)

# For lossy targets, pass codec options explicitly so the encode settings are
# visible at the call site.
df.set_pixel_data(
    "JPEG2000",
    arr,
    options={"type": "j2k", "target_psnr": 45.0},
)

rgb = np.arange(2 * 2 * 3, dtype=np.uint8).reshape(2, 2, 3)
df.set_pixel_data(
    "JPEGBaseline8Bit",
    rgb,
    options={
        "type": "jpeg",
        "quality": 90,
        "color_space": "ybr",
        "subsampling": "422",
    },
)
# The resulting DICOM metadata uses PhotometricInterpretation=YBR_FULL_422.
```

### Parse and validate a Python options dict once, then reuse the context

```python
import dicomsdl as dicom

df = dicom.read_file("sample.dcm")

# create_encoder_context() parses and validates the Python options object here,
# once, before the repeated write loop starts.
j2k_ctx = dicom.create_encoder_context(
    "JPEG2000",
    options={
        "type": "j2k",
        "target_psnr": 45.0,
        "threads": 4,
        "color_transform": True,
    },
)

# Reuse the same validated transfer syntax and option set for repeated outputs.
for path in ("out_j2k_1.dcm", "out_j2k_2.dcm"):
    df.write_with_transfer_syntax(path, "JPEG2000", encoder_context=j2k_ctx)
```

### Inspect a configuration error before the encode loop starts

```python
import dicomsdl as dicom

try:
    dicom.create_encoder_context(
        "JPEG2000",
        options={
            "type": "j2k",
            "target_psnr": -1.0,
        },
    )
except ValueError as exc:
    # Invalid options fail here, before any long-running encode loop starts.
    print(exc)
```

### Write a different transfer syntax without mutating the source object

```python
import dicomsdl as dicom

df = dicom.read_file("sample.dcm")

# write_with_transfer_syntax() changes only the serialized output path.
# The in-memory DicomFile keeps its current transfer syntax and pixel state.
df.write_with_transfer_syntax("out_rle.dcm", "RLELossless", options="rle")
```

## Exceptions

**C++**

| API | Throws | Typical reasons |
| --- | --- | --- |
| `create_encoder_context(...)` / `EncoderContext::configure(...)` | `dicom::diag::DicomException` | The transfer syntax is invalid or unsupported for encode. In C++, most codec-option semantics are still validated later when an encode or transcode call configures the runtime encoder. |
| `set_pixel_data(...)` | `dicom::diag::DicomException` | Source layout and source bytes disagree, the encoder context is missing or mismatched, the encoder binding is unavailable, the backend rejects the current codec options or pixel layout, or the transfer-syntax metadata update fails after encode. |
| `set_transfer_syntax(...)` | `dicom::diag::DicomException` | Transfer syntax selection is invalid, the encoder context does not match the requested syntax, the transcode path is unsupported, or backend encode fails. |
| `write_with_transfer_syntax(...)` | `dicom::diag::DicomException` | Transfer syntax selection is invalid, the encoder context does not match the requested syntax, the transcode path is unsupported, backend encode fails, or file/stream output fails. |

The C++ encode message usually includes `status=...`, `stage=...`, and `reason=...`, with statuses such as `invalid_argument`, `unsupported`, `backend_error`, or `internal_error`.

**Python**

| API | Raises | Typical reasons |
| --- | --- | --- |
| `create_encoder_context(...)` | `TypeError`, `ValueError`, `RuntimeError` | `options` has the wrong container or value type, option keys or values are invalid, transfer syntax text is unknown, or the underlying C++ configuration step still fails. |
| `set_pixel_data(...)` | `TypeError`, `ValueError`, `RuntimeError` | `source` is not a supported buffer object, is not C-contiguous, inferred source shape or dtype is invalid, encode options are invalid, or the runtime encoder/dataset update fails. |
| `set_transfer_syntax(...)` | `TypeError`, `ValueError`, `RuntimeError` | Transfer syntax text is invalid, the `options` object type is wrong, option values are invalid, the encoder context mismatches the requested syntax, or the transcode path/backend fails. |
| `write_with_transfer_syntax(...)` | `TypeError`, `ValueError`, `RuntimeError` | Path or `options` type is invalid, transfer syntax text or option values are invalid, the encoder context mismatches the requested syntax, or write/transcode fails. |

## Notes

- In C++, `set_pixel_data()` reads native pixels from the `pixel::ConstPixelSpan` layout you provide. If the source bytes have row or frame spacing, the layout must describe that spacing accurately.
- In Python, `set_pixel_data()` expects a C-contiguous numeric buffer. Use `np.ascontiguousarray(...)` first if the array is currently strided or non-contiguous.
- `set_pixel_data()` rewrites the relevant image-pixel metadata such as `Rows`, `Columns`, `SamplesPerPixel`, `BitsAllocated`, `BitsStored`, `PhotometricInterpretation`, `NumberOfFrames`, and the transfer syntax state.
- `set_transfer_syntax()` mutates the in-memory `DicomFile`. `write_with_transfer_syntax()` is the better path when the goal is just a differently encoded output file or stream.
- Reuse `EncoderContext` when the same transfer syntax and codec options are applied repeatedly. In Python, `create_encoder_context(..., options=...)` also parses and validates the `options` object up front. In C++, `EncoderContext` keeps one transfer syntax and option set together, while detailed failures still surface as `dicom::diag::DicomException`.
- For exact codec rules, option names, and per-transfer-syntax constraints, use the reference page instead of guessing from a short example.

## Related docs

- [Pixel Decode](pixel_decode.md)
- [File I/O](file_io.md)
- [Pixel Encode Constraints](../reference/pixel_encode_constraints.md)
