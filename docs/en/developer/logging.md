# dicomsdl Logging Principles

Short, fielded, single-line messages to aid reproducibility.

## Format
- `[LEVEL] context KEY=VALUE ... reason`
- Avoid multiline; no binary dumps. Use debug-only helpers for hex dumps if needed.

## Required fields during DICOM parsing
- `file=<path>` (relative if possible)
- `offset=0x%X` (byte offset in stream)
- `tag=(ggggeeee)` or `(gggg,eeee)`
- `vr=<VR>` when known; `length=<n>` when relevant
- Transport details when mismatched: `xfer=<uid>`, `endian=<little|big>`

## Severity
- **Error**: fatal; operation will throw/abort.
- **Warning**: recoverable; parsing continues.
- **Info**: phase/summary; compiled out unless `DICOMSDL_ENABLE_INFO_LOGS=1`.

## Error keywords (prefix after context)
- `INVALID_TAG`, `UNEXPECTED_EOF`, `BAD_VR`, `LENGTH_MISMATCH`, `CHARSET_UNSUPPORTED`, `UID_UNKNOWN`.
Example: `[ERROR] parse_header INVALID_TAG offset=0x1A3F tag=00100010 reason="non-hex digits"`  

## Throwing helpers
- Functions that log and then throw use `_or_throw` or `throw_...` in the name.
- They must emit exactly one `diag::error(...)` with the same core details used in the exception text.

## Concurrency / bindings
- Background threads add `thread=<id>`.
- Python entrypoints add `origin=python`.

## Performance guards
- Hot paths should guard with `meets_log_level()` before constructing messages.
- Keep string building minimal; prefer stack `ostringstream` or small fmt-style helpers.

## Version breadcrumb (optional)
- One startup Info line may include: `dicomsdl=<ver> DICOM=<stdver> build=<commit>`.
