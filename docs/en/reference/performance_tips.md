# Performance Tips

This page collects the stable public habits that usually matter most for dicomsdl performance.

## Use the API that returns exactly what you need

- typed read only: use `get_value(...)`
- typed read plus metadata: use `get_dataelement(...)` or `ds[...]`
- raw bytes: use `value_span()` when a view is enough

## Avoid unnecessary full traversal

- Prefer direct tag or keyword lookups over iterating the whole dataset in hot code.
- In C++, keep lazy loading targeted with `ensure_loaded(tag)` when you only need part of a file-backed dataset.

## Reuse decode plans and output buffers

- Build a decode plan once for a stable pixel layout.
- Reuse a preallocated destination with `decode_into(...)` when decoding many frames with the same decoded array dimensions.
- Recompute the plan after changing transfer syntax, rows, columns, samples per pixel, planar configuration, bit depth, or pixel data.

## Prefer streaming writes for large transcodes

- If your end goal is an output file or stream, prefer `write_with_transfer_syntax(...)` in C++ over mutating the in-memory object first. This can reduce peak memory use by avoiding a path that keeps decode working buffers and the re-encoded target `PixelData` on the object longer than needed.

## Respect view lifetimes

- `std::string_view`, `memoryview`, and raw byte spans are fast because they avoid copies.
- Those views become invalid after the owning element is replaced, removed, or rewritten.

## Batch writes with explicit error handling

- `set_value(...)` returns `false` on assignment failure.
- In C++, pair bulk writes with a `BufferingReporter` when you need detailed diagnostics without stopping at the first failure.

## Related docs

- [Python API Reference](python_reference.md)
- [C++ API Overview](cpp_api.md)
- [Pixel Reference](pixel_reference.md)
- [Error Model](error_model.md)
