# Pixel Decode

Use `create_decode_plan()` together with `decode_into()` when you want a validated decode layout before decoding, need to allocate or reuse your own output buffer, need explicit decoded row or frame strides, or want one code path for both single-frame and multi-frame inputs. Use `pixel_buffer()` in C++ and `to_array()` in Python for the simplest paths that return a new decoded result.

## Key Decode APIs

**C++**

- `create_decode_plan(...)` + `decode_into(...)`
  - Use this pair when you want a validated reusable decode layout together with a caller-provided output buffer, including single-frame cases where you want to size or reuse that buffer up front or request explicit output strides through `DecodeOptions`.
- `pixel_buffer(...)`
  - Decode and return a new pixel buffer.

**Python**

- `create_decode_plan(...)` + `decode_into(...)`
  - Use this pair when you want a validated reusable decode layout together with a caller-provided writable array or buffer, including single-frame cases where you want to prepare that destination up front or request explicit output strides through `DecodeOptions`.
- `to_array(...)`
  - Decode and return a new NumPy array. This is the quickest first-success path.
- `to_array_view(...)`
  - Return a zero-copy NumPy view when the source Pixel Data uses an uncompressed transfer syntax.

## Relevant DICOM standard sections

- The pixel attributes that control rows, columns, samples per pixel, photometric interpretation, and Pixel Data live in [DICOM PS3.3 Section C.7.6.3, Image Pixel Module](https://dicom.nema.org/medical/dicom/current/output/chtml/part03/sect_C.7.6.3.html).
- Native versus encapsulated Pixel Data encoding is defined in [DICOM PS3.5 Chapter 8, Encoding of Pixel, Overlay and Waveform Data](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/chapter_8.html) and [Section 8.2, Native or Encapsulated Format Encoding](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/sect_8.2.html).
- Encapsulated fragment/item layout and transfer syntax requirements are defined in [DICOM PS3.5 Section A.4, Transfer Syntaxes for Encapsulation of Encoded Pixel Data](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/sect_A.4.html).
- In file-based workflows, the Transfer Syntax UID comes from the file meta information described in [DICOM PS3.10 Chapter 7, DICOM File Format](https://dicom.nema.org/medical/dicom/current/output/chtml/part10/chapter_7.html).

## C++

### C++: Inspect the output layout before decoding one frame

```cpp
#include <cstdint>
#include <dicom.h>
#include <span>
#include <vector>

auto file = dicom::read_file("single_frame.dcm");

// The plan does not hold decoded pixels.
// Instead, it validates the current file metadata and reports what the
// decoded output must look like before we allocate any destination memory.
const auto plan = file->create_decode_plan();

// For single-frame decode, frame_stride is the exact byte count that
// decode_into() expects for one decoded frame with this plan.
std::vector<std::uint8_t> out(plan.output_layout.frame_stride);

// Frame 0 is the only frame here, but this call shape also works for
// multi-frame inputs. That makes it easy to keep one caller-owned buffer path
// for both single-frame and multi-frame code.
file->decode_into(0, std::span<std::uint8_t>(out), plan);

// `out` now contains one decoded frame laid out exactly as described by the plan.
```

### Reuse one plan and one destination buffer across many frames

```cpp
#include <cstdint>
#include <dicom.h>
#include <span>
#include <vector>

auto file = dicom::read_file("multiframe.dcm");
const auto plan = file->create_decode_plan();

// One DecodePlan implies one decoded frame layout, so we can allocate a single
// reusable frame buffer and refill it for each frame.
std::vector<std::uint8_t> frame_bytes(plan.output_layout.frame_stride);

for (std::size_t frame = 0; frame < plan.output_layout.frames; ++frame) {
	// Reuse the same validated layout for every frame instead of recalculating
	// metadata or allocating a fresh buffer each time.
	file->decode_into(frame, std::span<std::uint8_t>(frame_bytes), plan);

	// Process, copy, or forward `frame_bytes` here before the next iteration
	// overwrites it with the next decoded frame.
}
```

### C++: Build a plan from DecodeOptions

```cpp
#include <cstdint>
#include <dicom.h>
#include <span>
#include <vector>

auto file = dicom::read_file("multiframe_j2k.dcm");

dicom::pixel::DecodeOptions options{};
options.alignment = 32;
// Ask for planar output when the decoded image has multiple samples per pixel.
options.planar_out = dicom::pixel::Planar::planar;
// Apply the codestream-level inverse MCT/color transform when the backend
// supports it. This is the default and the usual starting point.
options.decode_mct = true;
// Outer worker scheduling matters mainly for batch or multi-work-item decode.
options.worker_threads = 4;
// Ask the codec backend to use up to two internal threads when supported.
options.codec_threads = 2;

// The plan captures these options together with the exact output layout they imply.
const auto plan = file->create_decode_plan(options);

// For full-volume decode, allocate enough storage for every decoded frame.
std::vector<std::uint8_t> volume(
    plan.output_layout.frames * plan.output_layout.frame_stride);

// decode_all_frames_into() uses the same validated plan, but fills the entire
// output volume instead of one frame at a time.
file->decode_all_frames_into(std::span<std::uint8_t>(volume), plan);
```

### C++: Request explicit output strides

```cpp
#include <cstdint>
#include <dicom.h>
#include <span>
#include <vector>

auto file = dicom::read_file("multiframe.dcm");

const auto rows = static_cast<std::size_t>(file["Rows"_tag].to_long().value_or(0));
const auto cols = static_cast<std::size_t>(file["Columns"_tag].to_long().value_or(0));
const auto samples_per_pixel =
    static_cast<std::size_t>(file["SamplesPerPixel"_tag].to_long().value_or(1));
const auto frame_count =
    static_cast<std::size_t>(file["NumberOfFrames"_tag].to_long().value_or(1));
const auto bits_allocated =
    static_cast<std::size_t>(file["BitsAllocated"_tag].to_long().value_or(0));
const auto bytes_per_sample = (bits_allocated + 7) / 8;
const auto packed_row_bytes = cols * samples_per_pixel * bytes_per_sample;
const auto row_stride = ((packed_row_bytes + 32 + 31) / 32) * 32;
const auto frame_stride = row_stride * rows;

// Allocate the destination buffers first from the metadata-derived layout.
std::vector<std::uint8_t> frame_bytes(frame_stride);
std::vector<std::uint8_t> volume_bytes(frame_count * frame_stride);

dicom::pixel::DecodeOptions options{};
// Interleaved output is the default, but spell it out here because the stride
// calculation below assumes interleaved samples within each row.
options.planar_out = dicom::pixel::Planar::interleaved;
// Add at least 32 bytes beyond the packed row payload, then round up to the
// next 32-byte boundary.
options.row_stride = row_stride;
options.frame_stride = frame_stride;

const auto plan = file->create_decode_plan(options);

// The plan validates the explicit row/frame strides we chose above.
file->decode_into(0, std::span<std::uint8_t>(frame_bytes), plan);
file->decode_all_frames_into(std::span<std::uint8_t>(volume_bytes), plan);
```

## Python

### Python: Inspect the output layout before decoding one frame

```python
import dicomsdl as dicom
import numpy as np

df = dicom.read_file("single_frame.dcm")

# The plan gives the decoded dtype and array shape before any pixels are decoded.
# That is useful when the caller wants to allocate the destination array first.
plan = df.create_decode_plan()

# Use the plan to get the exact NumPy array shape for one decoded frame.
# Using the plan keeps this allocation in sync with the later decode_into() call.
out = np.empty(plan.shape(frame=0), dtype=plan.dtype)

# Reuse the already-validated plan instead of recomputing layout metadata here.
df.decode_into(out, frame=0, plan=plan)

# `out` now contains one decoded frame with the layout requested by the plan.
```

### Python: Return `DecodeInfo` together with decoded pixels

```python
import dicomsdl as dicom
import numpy as np

df = dicom.read_file("single_frame.dcm")

# Ask to_array() for both the decoded NumPy array and decode metadata.
arr, info = df.to_array(frame=0, with_info=True)
print(arr.shape, arr.dtype)
print(info.photometric, info.encoded_lossy_state, info.bits_per_sample)

plan = df.create_decode_plan()
out = np.empty(plan.shape(frame=0), dtype=plan.dtype)

# decode_into() still writes into the caller-provided destination buffer.
# When with_info=True, the return value changes from `out` to DecodeInfo.
info2 = df.decode_into(out, frame=0, plan=plan, with_info=True)
assert np.array_equal(arr, out)
print(info2.photometric, info2.dtype, info2.planar)
```

`DecodeInfo` reports the successful decode result metadata:

- `photometric`: DICOM-level pixel interpretation when one is available
- `encoded_lossy_state`: `lossless`, `lossy`, `near_lossless`, or `unknown`
- `dtype`: NumPy dtype of the decoded samples
- `planar`: decoded planar/interleaved organization when known
- `bits_per_sample`: decoded bits per sample, or `0` when unknown

`DecodeInfo.photometric` follows the backend's actual decoded buffer domain, not
just stored metadata. For example, if a JPEG2000 or HTJ2K backend still returns
RGB-domain samples while `decode_mct=False`, `DecodeInfo.photometric` reports
`Photometric.rgb`.

For `frame=-1` on multi-frame input, Python `with_info=True` reports frame-0/common
decode metadata while returning the full decoded volume or filling the full destination
buffer.

### Reuse one plan and one destination array across many frames

```python
import dicomsdl as dicom
import numpy as np

df = dicom.read_file("multiframe.dcm")
plan = df.create_decode_plan()

# Each frame has the same decoded shape for one plan, so one reusable array is
# enough for a frame-by-frame processing loop.
frame_out = np.empty(plan.shape(frame=0), dtype=plan.dtype)

for frame in range(plan.frames):
    # decode_into() overwrites the same destination each time while reusing the
    # same validated plan.
    df.decode_into(frame_out, frame=frame, plan=plan)

    # Process, copy, or forward `frame_out` here before the next iteration
    # reuses it for the next decoded frame.
```

### Python: Build a plan from DecodeOptions

```python
import dicomsdl as dicom
import numpy as np

df = dicom.read_file("multiframe_j2k.dcm")

options = dicom.DecodeOptions(
    alignment=32,
    planar_out=dicom.Planar.planar,
    # Apply the codestream-level inverse MCT/color transform when the backend
    # supports it. This is the default and the usual starting point.
    decode_mct=True,
    # Outer worker scheduling matters mainly for batch or multi-frame decode.
    worker_threads=4,
    # Ask the codec backend to use up to two internal threads when supported.
    codec_threads=2,
)

# The plan captures the requested decode behavior, so later decode calls can
# just reuse `plan` without repeating the options.
plan = df.create_decode_plan(options)

# frame=-1 means "all frames". The plan can tell us the exact full-volume shape
# before we allocate the destination array.
volume = np.empty(plan.shape(frame=-1), dtype=plan.dtype)

# When plan=... is provided, the plan's captured options drive the decode.
df.decode_into(volume, frame=-1, plan=plan)
```

### Python: Request explicit output strides

```python
import dicomsdl as dicom
import numpy as np

df = dicom.read_file("test_le.dcm")

options = dicom.DecodeOptions(
    # Interleaved output is the default, but spell it out here because this
    # example is describing row strides in the interleaved layout.
    planar_out=dicom.Planar.interleaved,
    # For this tiny sample file, use a larger row stride to make the custom
    # layout obvious. For your own file, choose a value larger than the packed
    # decoded row size.
    row_stride=1024,
)
plan = df.create_decode_plan(options)

# to_array(plan=...) returns an array whose NumPy strides match the plan.
# That means the result may be intentionally non-contiguous when the plan uses
# explicit row or frame strides.
arr = df.to_array(frame=0, plan=plan)

# `arr.strides` now reflects the requested output strides, so the array
# may be intentionally non-contiguous even though the decoded pixel values are correct.
```

### Decode into raw storage for a custom-stride NumPy view

```python
import dicomsdl as dicom
import numpy as np

df = dicom.read_file("test_le.dcm")
plan = df.create_decode_plan(
    # This tiny sample uses a deliberately oversized row stride so the custom
    # NumPy view is easy to see. For your own file, pick a value that is large
    # enough for one decoded row.
    dicom.DecodeOptions(row_stride=1024)
)

# decode_into() still requires a writable C-contiguous output buffer object.
# For custom-stride layouts, allocate a raw 1-D buffer with exactly the number
# of decoded bytes that the plan requires.
raw = np.empty(
    plan.required_bytes(frame=0) // plan.bytes_per_sample,
    dtype=plan.dtype,
)
df.decode_into(raw, frame=0, plan=plan)

# Wrap the raw storage in a NumPy view whose explicit strides match the plan.
# This single-frame monochrome example becomes a custom-stride 2-D array view without
# an extra pixel copy.
arr = np.ndarray(
    shape=plan.shape(frame=0),
    dtype=plan.dtype,
    buffer=raw,
    strides=(plan.row_stride, plan.bytes_per_sample),
)
```

### Prepare NumPy storage first, then decode multi-frame output into it

```python
import dicomsdl as dicom
import numpy as np

df = dicom.read_file("multiframe.dcm")

# This example assumes a monochrome uint16 multi-frame output layout.
# When the decoded dtype or sample layout is different, adjust these values first.
dtype = np.uint16
itemsize = np.dtype(dtype).itemsize
rows = int(df.Rows)
cols = int(df.Columns)
frame_count = int(df.NumberOfFrames)
packed_row_bytes = cols * itemsize
# Add at least 32 bytes beyond the packed row payload, then round up to the
# next 32-byte boundary.
row_stride = ((packed_row_bytes + 32 + 31) // 32) * 32
frame_stride = row_stride * rows

# Prepare the backing storage first as a normal 1-D C-contiguous NumPy array.
# This is the object that decode_into() will write into.
backing = np.empty((frame_stride * frame_count) // itemsize, dtype=dtype)

# Build the application-facing array view over that same storage before decode.
# This example uses a frame-major monochrome layout:
#   (frames, rows, cols) with strides (frame_stride, row_stride, itemsize).
frames = np.ndarray(
    shape=(frame_count, rows, cols),
    dtype=dtype,
    buffer=backing,
    strides=(frame_stride, row_stride, itemsize),
)

# Build a matching plan after the storage layout has already been decided.
plan = df.create_decode_plan(
    dicom.DecodeOptions(
        # Interleaved output is the default, but spell it out here because the
        # storage layout above was prepared for interleaved samples.
        planar_out=dicom.Planar.interleaved,
        row_stride=row_stride,
        frame_stride=frame_stride,
    )
)

# Confirm that the plan matches the NumPy layout we prepared manually.
assert plan.dtype == np.dtype(dtype)
assert plan.bytes_per_sample == itemsize
assert plan.shape(frame=-1) == frames.shape
assert plan.row_stride == row_stride
assert plan.frame_stride == frame_stride
assert plan.required_bytes(frame=-1) == backing.nbytes

# decode_into() still requires the destination object itself to be writable and
# C-contiguous. That is why we pass `backing` here.
df.decode_into(backing, frame=-1, plan=plan)

# `frames` now exposes the decoded pixels through the NumPy layout you prepared
# in advance, while `backing` continues to own the underlying storage.
```

### Handle C++ decode failures explicitly

```cpp
#include <dicom.h>
#include <diagnostics.h>
#include <iostream>
#include <span>
#include <vector>

try {
    auto file = dicom::read_file("single_frame.dcm");
    const auto plan = file->create_decode_plan();

    std::vector<std::uint8_t> out(plan.output_layout.frame_stride);
    file->decode_into(0, std::span<std::uint8_t>(out), plan);
} catch (const dicom::diag::DicomException& ex) {
    // The message usually includes status=..., stage=..., and reason=...
    // so one log line is often enough to see whether the failure came from
    // metadata validation, destination validation, decoder selection, or the
    // backend decode step itself.
    std::cerr << ex.what() << '\n';
}
```

### Handle Python decode failures explicitly

```python
import dicomsdl as dicom
import numpy as np

df = dicom.read_file("single_frame.dcm")

try:
    plan = df.create_decode_plan()
    out = np.empty(plan.shape(frame=0), dtype=plan.dtype)
    df.decode_into(out, frame=0, plan=plan)
except (TypeError, ValueError, IndexError) as exc:
    # Binding-side validation failures land here:
    # wrong buffer type, wrong output size, invalid frame index, and so on.
    print(exc)
except RuntimeError as exc:
    # RuntimeError usually means the underlying C++ decode path failed after
    # the Python arguments were accepted.
    print(exc)
```

## Exceptions

**C++**

| API | Throws | Typical reasons |
| --- | --- | --- |
| `create_decode_plan(...)` | `dicom::diag::DicomException` | Pixel metadata is missing or inconsistent, `alignment` is invalid, explicit `row_stride` / `frame_stride` is smaller than the decoded payload, or the output layout overflows. |
| `decode_into(...)` | `dicom::diag::DicomException` | The plan no longer matches the current file state, the frame index is out of range, the destination buffer is too small, the decoder binding is unavailable, or backend decode fails. |
| `pixel_buffer(...)` | `dicom::diag::DicomException` | The same failure modes as `decode_into(...)`, but on the owning-buffer convenience path. |
| `decode_all_frames_into(...)` | `dicom::diag::DicomException` | The full-volume destination is too small, frame metadata is invalid, the decoder binding is unavailable, backend decode fails, or an `ExecutionObserver` cancels the batch. |

The C++ decode message usually includes `status=...`, `stage=...`, and `reason=...`, with statuses such as `invalid_argument`, `unsupported`, `backend_error`, `cancelled`, or `internal_error`.

**Python**

| API | Raises | Typical reasons |
| --- | --- | --- |
| `create_decode_plan(...)` | `RuntimeError` | The underlying C++ plan creation fails because pixel metadata is missing, the requested output layout is invalid, or the decoded layout overflows. |
| `to_array(...)` | `ValueError`, `IndexError`, `RuntimeError` | `frame < -1`, invalid thread counts, frame index out of range, or an underlying decode failure after argument validation succeeds. |
| `decode_into(...)` | `TypeError`, `ValueError`, `IndexError`, `RuntimeError` | Destination is not a writable C-contiguous buffer, itemsize or total byte size does not match the decoded layout, frame index is out of range, or the underlying decode path fails. |
| `to_array_view(...)` | `ValueError`, `IndexError` | The source transfer syntax is compressed, multi-sample native data is not interleaved, no direct raw pixel view is available, or the frame index is out of range. |

## Notes

- Even for single-frame inputs, `DecodePlan` is useful when you want to inspect the output layout before decoding or reuse a destination buffer across calls.
- Treat `DecodePlan` as a validated output layout, not as a cache of decoded pixels.
- `DecodeOptions.row_stride` and `DecodeOptions.frame_stride` let you request explicit row and frame strides for decoded output. When either is non-zero, `alignment` is ignored.
- Explicit decoded strides must still be large enough for the decoded row or frame payload and aligned to the decoded sample size.
- If you mutate pixel-affecting metadata such as transfer syntax, rows, columns, samples per pixel, bits allocated, pixel representation, planar configuration, number of frames, or pixel data elements, do not reuse old decode layout assumptions.
- If pixel-affecting metadata changes, create a new `DecodePlan` and a matching output buffer before the next `decode_into()`.
- `decode_into()` is the right path for benchmark or hot-loop reuse scenarios, or when you want the same buffer-management flow for both single-frame and multi-frame inputs.
- In Python, `to_array(plan=...)` may return a NumPy array with custom strides instead of a packed C-contiguous array when the plan requests explicit row or frame strides.
- In Python, `decode_into()` requires a writable C-contiguous destination object. For custom-stride results, decode into contiguous backing storage and then expose it through a NumPy view with explicit strides.
- In Python, `to_array(..., with_info=True)` returns `(array, decode_info)` instead of only the array.
- In Python, `decode_into(..., with_info=True)` still writes into the supplied destination object, but returns `DecodeInfo` instead of echoing `out`.
- `to_array()` is the right path for the quickest first success.

## Related docs

- [Quickstart](quickstart.md)
- [Pixel Encode](pixel_encode.md)
- [Pixel Transform Metadata Resolution](../reference/pixel_transform_metadata.md)
