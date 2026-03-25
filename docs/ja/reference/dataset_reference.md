# DataSet Reference

```{note}
このページ本文はまだ英語の原文です。必要に応じて英語版を基準に参照してください。
```

`DataSet` is the metadata container in DicomSDL. A root dataset belongs to a `DicomFile`, and nested datasets appear as sequence items.

## Core lookup APIs

- `operator[]` / `__getitem__`: return a `DataElement`; keyword strings for a single tag and dotted tag paths are accepted where supported, and missing lookups return an object that evaluates to `False` instead of throwing
- Python keyword attributes such as `ds.Rows` / `df.PatientName`: top-level typed reads for standard DICOM keywords; valid missing keywords return `None`, while unknown keywords raise `AttributeError`
- `get_dataelement(...)`: same `DataElement` lookup as `operator[]`, but in named-method form for tags, keywords, and dotted tag paths
- `get_value(...)`: one-shot typed read when you do not need `DataElement` metadata
- `__contains__` in Python: presence test without fetching the element separately
- iteration: `begin/end` in C++, `for elem in ds` in Python

## Mutation APIs

- `add_dataelement(...)`: create or replace a leaf element
- `ensure_dataelement(...)`: return existing element or create the missing path
- `ensure_loaded(...)`: advance partial loading before later reads or writes. In C++, this takes a `Tag`; in Python, it also accepts a packed `int` or a keyword string for a single tag
- `remove_dataelement(...)`: remove a tag from the current dataset
- `set_value(...)`: one-shot typed assignment by tag, keyword, or dotted path

## Path and sequence behavior

- Single-tag lookups accept tags and keywords such as `Rows` or `(0010,0010)`.
- Nested lookups accept dotted tag paths such as `ReferencedStudySequence.0.ReferencedSOPInstanceUID`.
- `ensure_dataelement(...)` and `set_value(...)` can materialize missing sequence items for dotted paths.
- When `ensure_dataelement(...)` needs to materialize a dotted path under an existing non-sequence intermediate element, it can reset that intermediate element to `SQ`.
- Malformed traversal or reading through a non-sequence path still raises.

## C++-only attachment APIs

- `attach_to_file(...)`, `attach_to_memory(...)`: attach a root dataset to file-backed or memory-backed input
- `read_attached_stream(options)`: perform the initial attached parse; use `options.load_until` when you want to stop early
- `path()`, `stream()`, `transfer_syntax_uid()`: inspect backing stream state

## Important notes

- Missing and zero-length are different states. Check the returned `DataElement` when presence matters.
- On partially loaded datasets, reads do not implicitly continue into data elements that have not been parsed yet.
- On partially loaded datasets, `add_dataelement(...)`, `ensure_dataelement(...)`, and `set_value(...)` throw when the target data element has not been parsed yet.
- In Python, attribute access is the main recommendation for ordinary top-level reads by standard DICOM keyword. Use `get_value(...)` or explicit string/int/`Tag` keys when the key is dynamic, nested, private, or not a valid Python identifier.
- In Python, most user code reaches a dataset through `df.dataset` or a sequence item rather than constructing a detached `DataSet` directly.

## Related docs

- [DicomFile Reference](dicomfile_reference.md)
- [DataElement Reference](dataelement_reference.md)
- [Sequence Reference](sequence_reference.md)
- [Tag-path lookup semantics](tag_path_lookup.md)
- [C++ DataSet Guide](../guide/cpp_dataset_guide.md)
- [Python DataSet Guide](../guide/python_dataset_guide.md)
