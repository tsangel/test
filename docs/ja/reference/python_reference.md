# Python API Reference

```{note}
このページ本文はまだ英語の原文です。必要に応じて英語版を基準に参照してください。
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

- `DataSetSelection([...])` は canonicalized nested selection tree を構築します。
- `read_file_selected(path, selection, keep_on_error=None)` は、選択した tag と nested sequence child だけを保持する `DicomFile` をディスクから読みます。
- `read_bytes_selected(data, selection, name="<memory>", keep_on_error=None, copy=True)` は
  bytes-like object から同じ選択結果を読みます。
- `selection` には再利用用の `DataSetSelection` も渡せますし、one-shot 呼び出し用に
  leaf tag と `(tag, children)` pair からなる raw nested Python sequence を
  そのまま渡すこともできます。
- `TransferSyntaxUID` と `SpecificCharacterSet` は selection に書かなくても
  root level で常に考慮されます。
- `ReadOptions.load_until` は selected-read API には適用されません。
- private tag と unknown tag も selection 対象として使え、`"70531000"` のような explicit tag string も使えます。
- `SQ` だけを選択しても present な sequence と item count は保持されますが、
  child item dataset は空になり得ます。
- 選択領域の外にある malformed data は見えないままのことがあり、`has_error` と
  `error_message` は selected read が実際に訪問した領域だけを表します。

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
