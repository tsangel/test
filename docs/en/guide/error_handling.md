# Error Handling

This page collects the failure-handling patterns that are spread across the focused guides. Use it when you want one place that answers two questions quickly:

- Which APIs throw or raise?
- What should I do next when they fail?

## Failure Types

DicomSDL public APIs use three different failure styles:

- Throw / raise
  - High-level C++ read, write, decode, encode, and dataset-wide charset mutation APIs usually fail with `dicom::diag::DicomException`.
  - Python usually sees the same runtime failures as `RuntimeError`, with `TypeError`, `ValueError`, and `IndexError` used for binding-side argument validation.
- Return-value failure
  - Some element-level charset APIs are normal return-value APIs instead of exception APIs. They report failure with `false`, `None`, or empty `optional`.
- Partial success with recorded error state
  - `read_file(..., keep_on_error=True)` and `read_bytes(..., keep_on_error=True)` may still return a `DicomFile`, but the file is marked with `has_error` and `error_message`.

## Exception Handling Patterns

**C++**

```cpp
try {
    // high-level dicomsdl operation
} catch (const dicom::diag::DicomException& ex) {
    // user-facing DICOM, codec, or file-I/O failure
} catch (const std::exception& ex) {
    // lower-level argument/usage or platform failure
}
```

**Python**

```python
import dicomsdl as dicom

try:
    # high-level dicomsdl operation
    ...
except TypeError as exc:
    # wrong argument type or non-buffer/path-like misuse
    ...
except ValueError as exc:
    # invalid text option, invalid buffer/layout request, malformed call
    ...
except IndexError as exc:
    # frame or component index out of range
    ...
except RuntimeError as exc:
    # underlying C++ parse, decode, encode, transcode, or write failure
    ...
```

## File I/O

Use `keep_on_error=False` when any parse problem should reject the file immediately. Use `keep_on_error=True` when early metadata is still useful even if the file later proves malformed.

### `keep_on_error=False`: fail fast

- Use this for import pipelines, validation jobs, or any workflow where a malformed file should stop immediately.
- Treat any exception as "this file is not safe to keep processing".
- Log the path and the exception text, then quarantine, skip, or report the file.

### `keep_on_error=True`: keep what was already parsed

- Use this for crawlers, metadata indexing, triage tools, or repair tooling that can still benefit from early tags.
- After every permissive read, check `has_error` and `error_message` before trusting the result.
- If `has_error` is true, treat the object as partially read or tainted:
  - use only the metadata you intentionally wanted to recover
  - do not continue blindly into pixel decode, pixel encode, or write-back flows
  - reload strictly after repair when you need a fully trustworthy object
- `keep_on_error` is not a general "ignore all errors" switch. Path/open failures, invalid Python buffer arguments, and similar boundary errors still raise immediately.

### Examples

**C++**

```cpp
#include <dicom.h>
#include <diagnostics.h>
#include <iostream>

try {
    dicom::ReadOptions opts;
    opts.keep_on_error = true;

    auto file = dicom::read_file("in.dcm", opts);
    if (file->has_error()) {
        std::cerr << "partial read: " << file->error_message() << '\n';
        // Keep only the metadata you explicitly wanted to recover.
        // Do not continue into decode/transcode as if this were a clean file.
    }
} catch (const dicom::diag::DicomException& ex) {
    // File open failure or another boundary failure that keep_on_error
    // does not convert into a partial-return state.
    std::cerr << ex.what() << '\n';
}
```

**Python**

```python
import dicomsdl as dicom

df = dicom.read_file("in.dcm", keep_on_error=True)
if df.has_error:
    print("partial read:", df.error_message)
    # Use only the already-parsed metadata you were trying to inspect.
    # Reload strictly before decode/transcode/write workflows.
```

### Throw / raise capable file I/O APIs

| API family | C++ failure form | Python raises | Typical reasons |
| --- | --- | --- | --- |
| `read_file(...)` | `dicom::diag::DicomException` when strict read fails; with `keep_on_error=true`, parse failures are captured on the returned `DicomFile` instead | `TypeError`, `RuntimeError` | Path cannot be opened, strict parse fails, or the Python path argument is not `str` / `bytes` / `os.PathLike` |
| `read_bytes(...)` | `dicom::diag::DicomException` when strict read fails; with `keep_on_error=true`, parse failures are captured on the returned `DicomFile` instead | `TypeError`, `ValueError`, `RuntimeError` | Buffer is not 1-D contiguous bytes-like data, `copy=False` is used with non-byte elements, or parsing fails |
| `write_file(...)` | `dicom::diag::DicomException` | `TypeError`, `RuntimeError` | Output path is invalid, file open/flush fails, file meta rebuild fails, or the dataset cannot be serialized in its current state |
| `write_bytes(...)` | `dicom::diag::DicomException` | `RuntimeError` | File meta rebuild fails or the current dataset cannot be serialized cleanly |
| `write_with_transfer_syntax(...)` | `dicom::diag::DicomException` | `TypeError`, `ValueError`, `RuntimeError` | Output path is invalid, transfer syntax selection is invalid, encoder context/options mismatch the request, transcode fails, or output write fails |

## Pixel Decode

The safest decode pattern is:

1. create a `DecodePlan` up front
2. allocate the destination from that plan
3. reuse the same validated plan and destination layout across decode calls

When decode fails, assume one of three buckets first: bad caller arguments, stale layout assumptions, or real backend/runtime decode failure.

### What to do when decode fails

- If validation fails before decode starts:
  - check frame index, destination size, contiguity, and `DecodeOptions`
- If a previously good plan starts failing:
  - recreate the plan and destination after any pixel-affecting metadata change
- If runtime decode fails:
  - log the message and treat it as a file/codec problem, not just a shape problem
- In Python:
  - `TypeError`, `ValueError`, and `IndexError` usually mean your call arguments or requested layout are wrong
  - `RuntimeError` usually means the underlying decode path itself failed

### Throw / raise capable pixel decode APIs

| API family | C++ failure form | Python raises | Typical reasons |
| --- | --- | --- | --- |
| `create_decode_plan(...)` | `dicom::diag::DicomException` | `RuntimeError` | Pixel metadata is missing or inconsistent, explicit strides are invalid, or the requested decoded layout overflows |
| `decode_into(...)` | `dicom::diag::DicomException` | `TypeError`, `ValueError`, `IndexError`, `RuntimeError` | Frame index is invalid, destination is the wrong size or layout, the plan no longer matches the file state, or the decoder/backend fails |
| `pixel_buffer(...)` | `dicom::diag::DicomException` | not exposed directly | The same underlying decode failures as `decode_into(...)` on the owning-buffer convenience path |
| `decode_all_frames_into(...)` | `dicom::diag::DicomException` | covered by `decode_into(..., frame=-1)` and `to_array(frame=-1)` | Destination is too small, frame metadata is invalid, or batch decode/backend execution fails |
| `to_array(...)` | not applicable | `ValueError`, `IndexError`, `RuntimeError` | Invalid frame request, invalid decode option request, or underlying decode failure |
| `to_array_view(...)` | not applicable | `ValueError`, `IndexError` | Invalid frame request, compressed source data, or no compatible direct raw pixel view |

## Pixel Encode

The safest encode pattern is:

1. validate the target transfer syntax and options before a long loop
2. prefer `EncoderContext` when the same transfer syntax and option set repeat
3. prefer `write_with_transfer_syntax(...)` when the goal is just a differently encoded output file

### What to do when encode fails

- If failure happens while building an `EncoderContext`:
  - fix the transfer syntax or option set before starting the real encode loop
- If failure happens during `set_pixel_data(...)`:
  - first verify the source buffer shape, dtype, contiguity, and pixel metadata assumptions
- If failure happens during `set_transfer_syntax(...)`:
  - treat it as an in-memory transcode failure on the current object state
- If the goal is output only:
  - prefer `write_with_transfer_syntax(...)` so a failed transcode does not become your normal in-memory workflow

### Throw / raise capable pixel encode APIs

| API family | C++ failure form | Python raises | Typical reasons |
| --- | --- | --- | --- |
| `create_encoder_context(...)` / `EncoderContext::configure(...)` | `dicom::diag::DicomException` | `TypeError`, `ValueError`, `RuntimeError` | Transfer syntax is invalid, option keys/values are invalid, or runtime encoder configuration fails |
| `set_pixel_data(...)` | `dicom::diag::DicomException` | `TypeError`, `ValueError`, `RuntimeError` | Source buffer type/shape/layout is invalid, source bytes disagree with the declared layout, encoder selection fails, or encode/backend update fails |
| `set_transfer_syntax(...)` | `dicom::diag::DicomException` | `TypeError`, `ValueError`, `RuntimeError` | Transfer syntax selection is invalid, options/context mismatch the request, or the transcode/backend path fails |
| `write_with_transfer_syntax(...)` | `dicom::diag::DicomException` | `TypeError`, `ValueError`, `RuntimeError` | Invalid path or transfer syntax text, invalid options/context, unsupported transcode path, backend encode failure, or output write failure |

## Charset and Person Name

Charset handling mixes two styles on purpose:

- element-level read/write helpers mostly report ordinary failure with `None`, empty `optional`, or `false`
- dataset-wide charset mutation is a validation/transcode operation and throws / raises on failure

That difference is important when you decide whether a failure is just "this one text assignment failed" or "this whole dataset transcode should stop".

### What to do when charset work fails

- For `to_utf8_string()` / `to_person_name()`:
  - treat empty `optional` or `None` as "decode/parsing did not produce a usable value"
  - choose a replacement policy when you want best-effort text instead of strict failure
- For `from_utf8_view()` / `from_person_name()`:
  - treat `false` as "this write did not succeed under the current charset/policy"
  - use `return_replaced=True` in Python or `bool* out_replaced` in C++ when lossy replacement is acceptable and you want to know whether it happened
- For Python element-level helpers:
  - remember that invalid `errors=` text still raises `ValueError` before the normal return-value path is reached
- For `set_specific_charset()`:
  - use `strict` for validation or fail-fast cleanup
  - use `replace_unicode_escape` when you want the transcode to finish while leaving visible `(U+XXXX)` replacement text
  - if the current dataset may already contain wrongly declared raw bytes, use the troubleshooting flow instead of treating normal transcode as declaration repair

### Throw / raise capable charset APIs

| API family | C++ failure form | Python raises | Typical reasons |
| --- | --- | --- | --- |
| `set_specific_charset(...)` | `dicom::diag::DicomException` | `TypeError`, `ValueError`, `RuntimeError` | Charset declaration text is invalid, the policy text is invalid, source text cannot be transcoded under the chosen policy, or dataset-wide charset mutation fails |
| `set_declared_specific_charset(...)` | `dicom::diag::DicomException` | `TypeError`, `ValueError`, `RuntimeError` | Declaration argument is invalid or `(0008,0005)` cannot be updated consistently; use mainly for repair/troubleshooting flows |

### Charset APIs that do not throw for ordinary content failure

| API family | C++ failure form | Python failure form | Typical meaning |
| --- | --- | --- | --- |
| `to_utf8_string()` / `to_utf8_strings()` | empty `std::optional` | `None` or `(None, replaced)` | Wrong VR, charset decode failed, or no usable decoded text was produced |
| `to_person_name()` / `to_person_names()` | empty `std::optional` | `None` or `(None, replaced)` | Wrong VR, charset decode failed, or PN parsing failed after decode |
| `from_utf8_view()` / `from_utf8_views()` | `false` | `False` or `(False, replaced)` | The element write did not succeed under the current charset and error policy |
| `from_person_name()` / `from_person_names()` | `false` | `False` or `(False, replaced)` | The PN write did not succeed under the current charset and error policy |

## Which Strategy Should I Start With?

- A malformed file should stop the workflow immediately
  - Use strict `read_file(...)` / `read_bytes(...)`
- I want to recover metadata from malformed files
  - Use `keep_on_error=True`, then always inspect `has_error` and `error_message`
- I want caller-managed decode buffers or explicit output strides
  - Use `create_decode_plan(...)` plus `decode_into(...)`
- I want the simplest decode path first
  - Use `to_array()` in Python or `pixel_buffer()` in C++
- I am writing many outputs with the same encode configuration
  - Build one `EncoderContext`
- I just want a different output transfer syntax
  - Prefer `write_with_transfer_syntax(...)`
- I am mutating or transcoding text values across a dataset
  - Use `set_specific_charset(...)`
- I am reading or writing one text element and want ordinary yes/no failure
  - Use `to_utf8_string()` / `from_utf8_view()` and their PN variants

## Related Docs

- [File I/O](file_io.md)
- [Pixel Decode](pixel_decode.md)
- [Pixel Encode](pixel_encode.md)
- [Charset and Person Name](charset_and_person_name.md)
- [Troubleshooting](troubleshooting.md)
- [Error Model](../reference/error_model.md)
