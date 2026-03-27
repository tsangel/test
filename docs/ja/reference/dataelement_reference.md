# DataElement Reference

```{note}
このページ本文はまだ英語の原文です。必要に応じて英語版を基準に参照してください。
```

`DataElement` is the metadata-bearing leaf object in DicomSDL. It carries `tag`, `vr`, `length`, offset information, presence state, and typed/raw value access.

## Core properties

- identity and metadata: `tag`, `vr`, `length`, `offset`, `vm`
- presence: `is_present()`, `is_missing()`, boolean checks such as `if (elem)`
- raw bytes: zero-copy の `value_span()` と、コピーされた bytes を返す `value_bytes()`
- nested payloads: `sequence` / `pixel_sequence` in Python, `sequence()` / `pixel_sequence()` in C++

## Container access

- Use `elem.is_sequence` before `elem.sequence` in Python when the element may not be `SQ`.
- Use `elem.is_pixel_sequence` before `elem.pixel_sequence` in Python when the element may hold encapsulated pixel data.
- In C++, the corresponding accessors are `sequence()` / `pixel_sequence()`, with `as_sequence()` / `as_pixel_sequence()` available when you want explicit checked casts.

## Typed read helpers

- numeric: `to_int`, `to_long`, `to_longlong`, `to_double` and vector variants
- tags and UIDs: `to_tag`, `to_tag_vector`, `to_uid_string`, `to_transfer_syntax_uid`
- text: `to_string_view`, `to_string_views`, `to_utf8_string`, `to_utf8_strings`
- person names: `to_person_name`, `to_person_names`
- Python convenience: `.value` and `.get_value()` expose the binding-level decoded value path

## Typed write helpers

- Python:
  - `.value = ...`
  - `set_value(...)`
  - `from_utf8_view(...)`, `from_utf8_views(...)`
  - `from_person_name(...)`, `from_person_names(...)`
- C++:
  - `set_value_bytes(...)`, `adopt_value_bytes(...)`, `reserve_value_bytes(...)`
  - `from_int`, `from_long`, `from_longlong`, `from_double`
  - `from_tag`, `from_string_view`, `from_string_views`
  - `from_utf8_view`, `from_utf8_views`
  - `from_uid`, `from_uid_string`, `from_transfer_syntax_uid`, `from_sop_class_uid`
  - `from_person_name`, `from_person_names`

## Important notes

- A missing element and a present zero-length element are not the same thing.
- `value_span()` and `to_string_view()` style accessors are view-based. Their results become invalid if the element is replaced or mutated.
- In Python, copied bytes are often a better fit for ownership-sensitive code or very small payloads. A rough guide from current binding benchmarks is copied bytes up to about `2 KiB`, `value_span()` from about `4 KiB`, and a strong preference for `value_span()` at `64 KiB+`.
- `to_string_views()` can fail for multibyte declared charsets where splitting raw bytes on `\\` would be unsafe.
- Sequence and pixel-sequence elements are container values. Treat them as `Sequence` / `PixelSequence`, not as scalar strings or numbers.

## Related docs

- [DataSet Reference](dataset_reference.md)
- [Sequence Reference](sequence_reference.md)
- [Charset Reference](charset_reference.md)
- [Python API Reference](python_reference.md)
- [C++ API Overview](cpp_api.md)
