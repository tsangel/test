# Charset Reference

This page summarizes the public charset and person-name APIs used to decode and encode text safely.

## File and dataset level APIs

- Python `DicomFile`:
  - `set_declared_specific_charset(...)`
  - `set_specific_charset(...)`
- C++ `DicomFile` and `DataSet`:
  - `set_declared_specific_charset(...)`
  - `set_specific_charset(...)`

Use the declared-charset API when you only need to update `(0008,0005)` metadata. Use the transcode API when you want existing text values rewritten to a new charset.

## Element-level text APIs

- raw-ish string helpers:
  - `to_string_view()`
  - `to_string_views()`
  - `from_string_view(...)`
  - `from_string_views(...)`
- charset-aware UTF-8 helpers:
  - `to_utf8_string(...)`
  - `to_utf8_strings(...)`
  - `from_utf8_view(...)`
  - `from_utf8_views(...)`
- person name helpers:
  - `to_person_name(...)`
  - `to_person_names(...)`
  - `from_person_name(...)`
  - `from_person_names(...)`

## Important rules

- Do not edit raw `(0008,0005) Specific Character Set` bytes directly unless you are intentionally bypassing charset cache management.
- `to_string_views()` is a view-based helper and can return no value for multibyte declared charsets where raw byte splitting would be unsafe.
- Use the UTF-8 helpers when you want stable text behavior across ISO 2022, GBK, GB18030, and other multibyte declarations.
- In Python, the charset APIs can report whether replacement happened through the `return_replaced` option.

## Related docs

- [Charset and Person Name](../guide/charset_and_person_name.md)
- [DataElement Reference](dataelement_reference.md)
- [Error Model](error_model.md)
