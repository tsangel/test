# Error Model

```{note}
このページ本文はまだ英語の原文です。必要に応じて英語版を基準に参照してください。
```

This page describes which exception categories DicomSDL may throw, what they usually mean, and what public API consumers should rely on.

## Short version

- High-level DICOM operations usually fail with `dicom::diag::DicomException`.
- Low-level value types and utility helpers may still throw standard C++ exceptions.
- If you are calling public read, write, decode, or encode entrypoints, catching `const dicom::diag::DicomException&` is usually enough.
- If you are using low-level helpers directly, catch `const std::exception&` unless you want to handle categories more precisely.

## Main exception categories

### `dicom::diag::DicomException`

This is the main DicomSDL domain exception.

Use cases:

- DICOM parsing failures
- DICOM write / transcode failures
- Pixel decode / encode failures
- Boundary-level errors where DicomSDL attaches DICOM-specific context such as file path, transfer syntax, frame index, stage, or reason

Typical message format:

```text
operation file=<...> ts=<...> frame=<...> stage=<...> reason=<...>
```

Not every field is present in every message, but `reason=` should always be present in user-facing failures.

### `std::invalid_argument`

Used for invalid inputs, malformed configuration, or argument/precondition violations in low-level helpers and value types.

Common examples:

- unknown DICOM keyword or UID
- invalid pixel layout
- unsupported LUT metadata
- invalid transform arguments

### `std::out_of_range`

Used when an index or byte range is outside the valid bounds.

Common examples:

- frame index out of range
- span access out of range
- stream seek / slice range out of range

### `std::overflow_error`

Used when a size, stride, length, or byte-count computation overflows.

Common examples:

- pixel row / frame stride overflow
- buffer size overflow
- transform layout arithmetic overflow

### `std::system_error`

Used for operating system and filesystem failures.

Common examples:

- file open failure
- memory mapping failure
- OS-backed I/O setup failure

### `std::logic_error`

Used for programmer misuse or an invalid internal state in utility classes.

Common examples:

- accessing an `InStream` that has no backing data

## Public API guidance

For public DicomSDL operations, prefer this pattern:

```cpp
try {
  // high-level dicomsdl call
} catch (const dicom::diag::DicomException& ex) {
  // user-facing DICOM failure with dicomsdl context
} catch (const std::exception& ex) {
  // lower-level or platform-specific failure
}
```

If your application only uses high-level read / write / decode / encode entrypoints, you may choose to catch only `dicom::diag::DicomException` and let unexpected standard exceptions propagate as bugs or infrastructure failures.

If your application uses low-level layout, span, stream, or transform utilities directly, be prepared for standard exceptions as part of their normal API behavior.

## Stability expectations

The following are intended to be stable at a high level:

- high-level DICOM failures primarily use `dicom::diag::DicomException`
- low-level generic failures may use standard C++ exceptions
- public boundary messages include DICOM-specific context when available

The following should be treated as implementation details and may change over time:

- exact English wording
- the full set and order of message fields
- which helper throws at which exact stack depth

Applications should not parse full exception messages for control flow. If you need structured handling, treat exception category as the primary signal and the message as diagnostic text.

## Relationship to logging

DicomSDL error messages are designed so that the final exception text and the final log message can match closely.

See [logging](../developer/logging.md) for logging format and boundary logging principles.
