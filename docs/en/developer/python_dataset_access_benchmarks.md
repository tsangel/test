# Python DataSet access quick notes (0.1.4 vs 0.1.5)

Note: these measurements were taken before the Python binding switched
`DataSet.__getitem__` to return `DataElement`. In the current API,
`ds["Rows"]` measures element retrieval, while one-shot scalar access is
represented by `ds.get_value("Rows")` or `ds["Rows"].value`.

Benchmarks (Apple M3 / Python 3.12, sample: `1.2.840.113619.2.99.1234.1210123180.675655_0000_000034_121066021209fb.dcm`)

## v0.1.4 (pybind11) — average of 5 runs

### v0.1.4 scalar access: Rows (0x0028,0010), 100k iterations each

| Rank | Access pattern | us / call |
| --- | --- | --- |
| 1 | `ds[0x00280010]` | 0.31 |
| 2 | `ds.Rows` | 0.32 |
| 3 | `ds["Rows"]` | 0.33 |
| 4 | `ds.get_dataelement(0x00280010).to_long()` | 0.78 |
| 5 | `ds.get_dataelement(0x00280010).get_value()` | 0.78 |

### v0.1.4 pydicom comparison (Rows, 0x0028,0010), 100k iterations

| Rank | Access pattern | us / call |
| --- | --- | --- |
| 1 | `pydicom get_item((0x0028,0x0010)).value` | 0.73 |
| 2 | `pydicom int(get_item((0x0028,0x0010)).value)` | 0.77 |
| 3 | `pydicom ds[(0x0028,0x0010)].value` | 0.82 |
| 4 | `pydicom int(ds[(0x0028,0x0010)].value)` | 0.91 |
| 5 | `pydicom ds.Rows` | 0.98 |

### v0.1.4 sequence path, 5,000 iterations

| Rank | Access pattern | us / call |
| --- | --- | --- |
| 1 | `ds["00540016.0.00181075"]` | 0.46 |
| 2 | `ds["RadiopharmaceuticalInformationSequence.0.RadionuclideHalfLife"]` | 0.68 |
| 3 | `ds.RadiopharmaceuticalInformationSequence[0].RadionuclideHalfLife` | 1.28 |
| 4 | `ds.get_dataelement(0x00540016).sequence[0].get_dataelement(0x00181075).to_long()` | 2.16 |
| 5 | `ds.get_dataelement(0x00540016).sequence[0].get_dataelement(0x00181075).get_value()` | 2.16 |

## v0.1.5 (nanobind) — average of 5 runs

### v0.1.5 scalar access: Rows (0x0028,0010), 100k iterations each

| Rank | Access pattern | us / call |
| --- | --- | --- |
| 1 | `ds[0x00280010]` | 0.11 |
| 2 | `ds["Rows"]` | 0.14 |
| 3 | `ds.Rows` | 0.15 |
| 4 | `ds.get_dataelement(0x00280010).get_value()` | 0.16 |
| 5 | `ds.get_dataelement(0x00280010).to_long()` | 0.20 |

### v0.1.5 pydicom comparison (Rows, 0x0028,0x0010), 100k iterations

| Rank | Access pattern | us / call |
| --- | --- | --- |
| 1 | `pydicom int(get_item((0x0028,0x0010)).value)` | 0.78 |
| 2 | `pydicom get_item((0x0028,0x0010)).value` | 0.81 |
| 3 | `pydicom ds[(0x0028,0x0010)].value` | 0.83 |
| 4 | `pydicom int(ds[(0x0028,0x0010)].value)` | 0.90 |
| 5 | `pydicom ds.Rows` | 0.98 |

### v0.1.5 sequence path, 5,000 iterations

| Rank | Access pattern | us / call |
| --- | --- | --- |
| 1 | `ds["00540016.0.00181075"]` | 0.27 |
| 2 | `ds["RadiopharmaceuticalInformationSequence.0.RadionuclideHalfLife"]` | 0.49 |
| 3 | `ds.get_dataelement(0x00540016).sequence[0].get_dataelement(0x00181075).get_value()` | 0.49 |
| 4 | `ds.get_dataelement(0x00540016).sequence[0].get_dataelement(0x00181075).to_long()` | 0.51 |
| 5 | `ds.RadiopharmaceuticalInformationSequence[0].RadionuclideHalfLife` | 0.63 |

## dicomsdl change summary (0.1.4 -> 0.1.5)

| Access pattern | v0.1.4 us/call | v0.1.5 us/call | Speedup (x) |
| --- | --- | --- | --- |
| `ds[0x00280010]` | 0.31 | 0.11 | 2.73 |
| `ds["Rows"]` | 0.33 | 0.14 | 2.33 |
| `ds.Rows` | 0.32 | 0.15 | 2.08 |
| `get_dataelement(...).to_long()` | 0.78 | 0.20 | 3.85 |
| `get_dataelement(...).get_value()` | 0.78 | 0.16 | 4.81 |
| packed sequence path | 0.46 | 0.27 | 1.72 |
| keyword sequence path | 0.68 | 0.49 | 1.40 |
| attribute chain | 1.28 | 0.63 | 2.03 |
| chained `to_long()` | 2.16 | 0.51 | 4.25 |
| chained `get_value()` | 2.16 | 0.49 | 4.45 |

Takeaway: this 5-run sample shows clear end-to-end improvement in `dicomsdl` access paths after moving from v0.1.4 to v0.1.5, while `pydicom` stays roughly in the same range across runs.

## nanobind build mode A/B (Regular vs STABLE_ABI)

Setup:
- Runtime: CPython 3.12.12 (Apple M3)
- Sample: `1.2.840.113619.2.99.1234.1210123180.675655_0000_000034_121066021209fb.dcm`
- Script: `benchmarks/python/benchmark_dataset_access.py`
- Method: each variant ran 6 times, first run discarded as warm-up, last 5 averaged
- Variants:
  - Regular: `nanobind_add_module(... NOMINSIZE ...)`
  - STABLE_ABI: `nanobind_add_module(... STABLE_ABI NOMINSIZE ...)` (`abi3`)

| Access pattern | Regular us/call | STABLE_ABI us/call | STABLE_ABI overhead |
| --- | --- | --- | --- |
| `ds.Rows` | 0.114 | 0.122 | +7.0% |
| `ds[0x00280010]` | 0.094 | 0.110 | +17.0% |
| `ds["Rows"]` | 0.130 | 0.136 | +4.6% |
| `get_dataelement(...).to_long()` | 0.180 | 0.198 | +10.0% |
| `get_dataelement(...).get_value()` | 0.160 | 0.172 | +7.5% |
| packed sequence path | 0.276 | 0.290 | +5.1% |
| keyword sequence path | 0.532 | 0.532 | +0.0% |
| attribute chain | 0.676 | 0.704 | +4.1% |
| chained `to_long()` | 0.478 | 0.562 | +17.6% |
| chained `get_value()` | 0.478 | 0.542 | +13.4% |

Takeaway: in this project, `STABLE_ABI` is generally slower in microbenchmarks (roughly +5~10% typical, up to ~+18% on some chained/scalar paths), while improving wheel compatibility across Python minor versions.

## Current branch notes (v0.1.35)

This note now sits on top of a newer API surface than the original
`0.1.4 vs 0.1.5` comparison above. The tables remain useful for the
historical pybind11 -> nanobind transition, but they no longer cover the
full set of current `DataSet` access patterns.

Key updates closely related to Python `DataSet` access:

- `DataSet.__getitem__` still returns `DataElement`, so one-shot scalar access
  should be read as `ds.get_value("Rows")` or `ds["Rows"].value`.
- String tag-path writes are now first-class operations:
  - `ds.ensure_dataelement("Seq.0.Tag", vr)`
  - `ds.add_dataelement("Seq.0.Tag", vr)`
  - `ds.set_value("Seq.0.Tag", value)`
- Path traversal code was refactored so read/write paths share the same parsing
  model, and flat string paths avoid the heavier dotted-path loop when no `.`
  is present.
- Runtime keyword/tag lookup is substantially faster than the versions measured
  above because the CHD lookup path now has dedicated runtime fast paths and
  smaller caches tuned for the actual access patterns in this project.
- Numeric tag text such as `00100010` and `(0010,0010)` now bypasses the
  keyword miss path on the common fast forms.

Behavioral changes that matter when reading old benchmark tables:

- Partial-load mutation is now stricter. Python `get_value()`, `set_value()`,
  `add_dataelement()`, and `ensure_dataelement()` do not silently load future
  tags. Access beyond `last_tag_loaded_` raises instead of mutating unread
  tail state.
- DICOM assignment errors raised from Python attribute assignment convenience access are no longer
  swallowed and re-labeled as generic `AttributeError`.
- Binary `memoryview` results returned from `.value` / `get_value()` now keep
  the owning object alive, so the Python side does not end up with an
  owner-less view into released storage.

Practical takeaway for the current version:

- If readability matters, `ds["Rows"].value`, `ds.get_value("Rows")`, and
  dotted string paths are now a reasonable default.
- If a call site is extremely hot, direct `Tag` / integer access is still the
  lowest-overhead form.
- The historical tables above should be interpreted as transition-era numbers,
  not as current absolute timings for `0.1.35`.

## v0.1.35 current release-style sample

Runtime:
- CPython 3.12
- Apple M3
- Release `_dicomsdl`
- Sample: `1.2.840.113619.2.99.1234.1210123180.675655_0000_000034_121066021209fb.dcm`

### Scalars

| Rank | Access pattern | us / call |
| --- | --- | --- |
| 1 | `ds.get_value(0x00280010)` | 0.11 |
| 2 | `ds.Rows` | 0.12 |
| 3 | `ds.get_value("Rows")` | 0.14 |
| 4 | `ds[0x00280010]` | 0.14 |
| 5 | `ds["Rows"]` | 0.16 |
| 6 | `ds[0x00280010].value` | 0.17 |
| 7 | `get_dataelement(...).get_value()` | 0.16 |
| 8 | `get_dataelement(...).to_long()` | 0.18 |
| 9 | `ds["Rows"].value` | 0.19 |

### Sequence / path

| Rank | Access pattern | us / call |
| --- | --- | --- |
| 1 | `ds["00540016.0.00181075"]` | 0.18 |
| 2 | `ds["RadiopharmaceuticalInformationSequence.0.RadionuclideHalfLife"]` | 0.24 |
| 3 | `ds.get_value(path)` | 0.32 |
| 4 | `ds[path].value` | 0.38 |
| 5 | chained `get_value()` | 0.46 |
| 6 | attribute chain | 0.48 |
| 7 | chained `to_long()` | 0.50 |

### Sequence / path, cache-disabled experiment

Setup:
- Same synthetic `DataSet` as the sequence/path table above
- 5 runs averaged
- 5,000 iterations per access pattern
- Runtime keyword cache temporarily disabled for measurement

| Rank | Access pattern | us / call |
| --- | --- | --- |
| 1 | `ds["00540016.0.00181075"]` | 0.27 |
| 2 | `ds["RadiopharmaceuticalInformationSequence.0.RadionuclideHalfLife"]` | 0.34 |
| 3 | `ds.RadiopharmaceuticalInformationSequence[0].RadionuclideHalfLife` | 0.52 |
| 4 | `ds.get_dataelement(0x00540016).sequence[0].get_dataelement(0x00181075).to_long()` | 0.54 |
| 5 | `ds.get_dataelement(0x00540016).sequence[0].get_dataelement(0x00181075).get_value()` | 0.58 |

### Sequence path historical comparison

This keeps the original `0.1.4` and `0.1.5` sequence-path measurements and
adds the current branch with the runtime keyword cache disabled for a cleaner
parser/path comparison.

| Access pattern | v0.1.4 us/call | v0.1.5 us/call | current uncached us/call |
| --- | --- | --- | --- |
| `ds["00540016.0.00181075"]` | 0.46 | 0.27 | 0.27 |
| `ds["RadiopharmaceuticalInformationSequence.0.RadionuclideHalfLife"]` | 0.68 | 0.49 | 0.34 |
| `ds.RadiopharmaceuticalInformationSequence[0].RadionuclideHalfLife` | 1.28 | 0.63 | 0.52 |
| `ds.get_dataelement(0x00540016).sequence[0].get_dataelement(0x00181075).to_long()` | 2.16 | 0.51 | 0.54 |
| `ds.get_dataelement(0x00540016).sequence[0].get_dataelement(0x00181075).get_value()` | 2.16 | 0.49 | 0.58 |

### Mutation

| Rank | Access pattern | us / call |
| --- | --- | --- |
| 1 | `elem.value = 512` | 0.08 |
| 2 | `ds.set_value("Rows", 512)` | 0.12 |
| 3 | `ensure("Rows").value = 512` | 0.15 |
| 4 | `path set_value` | 0.23 |
| 5 | `path ensure().value` | 0.32 |

## dicomsdl change summary (0.1.5 -> 0.1.35, current API)

The table below keeps only directly comparable access patterns. Paths that
used to rely on `__getitem__` returning a scalar are listed separately in the
next table using their current best equivalents.

| Access pattern | v0.1.5 us/call | v0.1.35 us/call | Speedup (x) |
| --- | --- | --- | --- |
| `ds.Rows` | 0.15 | 0.12 | 1.25 |
| `get_dataelement(...).to_long()` | 0.20 | 0.18 | 1.11 |
| `get_dataelement(...).get_value()` | 0.16 | 0.16 | 1.00 |
| attribute chain | 0.63 | 0.48 | 1.31 |
| chained `to_long()` | 0.51 | 0.50 | 1.02 |
| chained `get_value()` | 0.49 | 0.46 | 1.07 |

### Current equivalents for old scalar `__getitem__` patterns

`v0.1.5` measured `ds[tag]` and `ds["keyword"]` as one-shot scalar access.
In the current API, `__getitem__` returns `DataElement`, so the nearest scalar
equivalents are `get_value(...)` or `.value`.

| Historical v0.1.5 pattern | v0.1.5 us/call | Current equivalent | v0.1.35 us/call |
| --- | --- | --- | --- |
| `ds[0x00280010]` | 0.11 | `ds.get_value(0x00280010)` | 0.11 |
| `ds["Rows"]` | 0.14 | `ds.get_value("Rows")` | 0.14 |
| packed sequence path | 0.27 | `ds["00540016.0.00181075"]` element lookup | 0.18 |
| keyword sequence path | 0.49 | `ds.get_value(path)` | 0.32 |

Takeaway:

- On the current branch, the hot scalar read paths are again in the same range
  as the old `0.1.5` release measurements once `DicomFile` uses direct Python
  bindings instead of `__getattr__` forwarding.
- `ds.Rows` is now slightly faster than the old `0.1.5` sample.
- `get_dataelement(...).get_value()` is back to roughly the same level as the
  old `0.1.5` sample, and `get_dataelement(...).to_long()` is slightly faster.
