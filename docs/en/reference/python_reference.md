# Python API Reference

The user-facing Python guide lives in
[Python DataSet Guide](../guide/python_dataset_guide.md).

Use that page for:

- the recommended Python access patterns
- `DataSet` / `DicomFile` / `DataElement` behavior
- value reads and writes
- presence checks
- explicit VR assignment for private tags

## Selected read

- `DataSetSelection([...])` builds a canonicalized nested selection tree.
- `read_file_selected(path, selection, keep_on_error=None)` reads a `DicomFile`
  that keeps only the selected tags and nested sequence children.
- `read_bytes_selected(data, selection, name="<memory>", keep_on_error=None, copy=True)`
  reads the same selected result from a bytes-like object.
- `selection` may be a reusable `DataSetSelection` or a raw nested Python
  sequence of leaf tags and `(tag, children)` pairs for one-shot calls.
- `TransferSyntaxUID` and `SpecificCharacterSet` are always considered at the
  root level, even when omitted from the selection.
- `ReadOptions.load_until` does not apply to selected-read APIs.
- Private and unknown tags are valid selection targets, including explicit tag strings such as `"70531000"`.
- Selecting only an `SQ` keeps the present sequence and item count, but child
  item datasets may be empty.
- Malformed data outside the selected region may remain unseen, so `has_error`
  and `error_message` only describe the region that selected read actually visits.

## Supporting types

### Tag

Construct from:

- `(group, element)`
- packed `int`
- keyword or Tag string

Important properties:

- `group`
- `element`
- `value`
- `is_private`

`str(tag)` renders as `(gggg,eeee)`.

### VR

Enum-like VR wrapper with constants such as `VR.AE`, `VR.US`, and `VR.UI`.

Useful helpers:

- `str(vr)` / `vr.str()`
- `is_string()`
- `is_binary()`
- `is_sequence()`
- `is_pixel_sequence()`
- `uses_specific_character_set()`
- `allows_multiple_text_values()`

### Uid

Construct from a keyword or dotted string.
Unknown values raise.

## Related docs

- [Python DataSet Guide](../guide/python_dataset_guide.md)
- [Selected Read](../guide/selected_read.md)
- [Charset and Person Name](../guide/charset_and_person_name.md)
