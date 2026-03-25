# Sequence Reference

```{note}
이 페이지 본문은 아직 영어 원문입니다. 필요하면 영문 페이지를 기준으로 읽어 주세요.
```

`Sequence` represents the value of an `SQ` data element. It is an ordered list of item `DataSet` objects.

## How you get a `Sequence`

- Python: `elem.is_sequence`, then `elem.sequence`
- C++: `elem.sequence()`, `elem.as_sequence()`, or `elem.storage_kind()`
- dotted tag paths such as `SeqKeyword.0.LeafKeyword` are shorthand for sequence-item traversal

## Core operations

- size/length: `len(seq)` in Python, `size()` in C++
- index access: `seq[index]`
- iteration: `for item in seq` in Python, iterators in C++
- mutation: `add_dataset()` appends a new item dataset

## Notes

- Each item is a normal `DataSet`, so the same lookup and write APIs apply once you are inside the item.
- `Sequence` is different from encapsulated pixel storage. Pixel fragments live in `PixelSequence`, not in `Sequence`.
- If you only need a single nested leaf, dotted tag paths are usually shorter than manual traversal.
- If you need to inspect or rewrite many items, keep the `Sequence` object and work item-by-item.

## Related docs

- [DataSet Reference](dataset_reference.md)
- [Tag-path lookup semantics](tag_path_lookup.md)
- [Sequence and Paths](../guide/sequence_and_paths.md)
