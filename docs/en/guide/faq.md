# FAQ

## Should I start from `DicomFile` or `DataSet`?

Start from `DicomFile` when you care about file/session state, pixel decode, or serialization. Start from `DataSet` when you are focused on metadata reads and writes.

## When should I use `get_value()` instead of `ds[...]`?

Use `get_value()` for one-shot typed reads. Use `ds[...]` when you also need `tag`, `vr`, `length`, or raw element metadata.

## Why does a missing element not always raise?

dicomsdl intentionally keeps `DataElement` access non-throwing in many places so you can test whether an element is present and chain lookups safely.

## Why do zero-length values look different from missing values?

Because zero-length means "present with empty stored content", while missing means the element is not there at all.

## Where should I look for nested sequence path rules?

See [Sequence and Paths](sequence_and_paths.md) and [Tag-path Lookup Semantics](../reference/tag_path_lookup.md).

## Where should I look for pixel encode limits?

See [Pixel Encode](pixel_encode.md) for the overview and [Pixel Encode Constraints](../reference/pixel_encode_constraints.md) for the exact requirements.
