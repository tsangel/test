# Python API Reference

```{note}
本页正文目前仍为英文原文。需要时请以英文版为准。
```

The user-facing Python guide lives in
[Python DataSet Guide](../guide/python_dataset_guide.md).

Use that page for:

- the recommended Python access patterns
- `DataSet` / `DicomFile` / `DataElement` behavior
- value reads and writes
- presence checks
- explicit VR assignment for private tags

## Selected read

- `DataSetSelection([...])` 会构造 canonicalized nested selection tree。
- `read_file_selected(path, selection, keep_on_error=None)` 会从磁盘读取只包含所选 tag 和嵌套 sequence child 的 `DicomFile`。
- `read_bytes_selected(data, selection, name="<memory>", keep_on_error=None, copy=True)` 会从
  bytes-like object 读取同样的选择结果。
- `selection` 既可以是可复用的 `DataSetSelection`，也可以是 one-shot 调用里直接传入的
  raw nested Python sequence，由 leaf tag 和 `(tag, children)` pair 组成。
- `TransferSyntaxUID` 和 `SpecificCharacterSet` 即使没有写进 selection，也始终会在
  root level 被考虑。
- `ReadOptions.load_until` 不适用于 selected-read API。
- private tag 和 unknown tag 也可以作为 selection 目标，也可以写成 `"70531000"` 这样的 explicit tag string。
- 即使只选择 `SQ` 本身，也会保留 present 的 sequence 和 item count，但 child item
  dataset 可能为空。
- 选择区域之外的 malformed data 可能完全不会被看到，因此 `has_error` 和
  `error_message` 只描述 selected read 实际访问到的区域。

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
