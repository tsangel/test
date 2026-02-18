# Python DataSet access quick notes (0.1.4 vs 0.1.5)

Benchmarks (Apple M3 / Python 3.12, sample: `1.2.840.113619.2.99.1234.1210123180.675655_0000_000034_121066021209fb.dcm`)

## v0.1.4 (pybind11) — average of 5 runs

### Scalars: Rows (0x0028,0010), 100k iterations each

| Rank | Access pattern | us / call |
| --- | --- | --- |
| 1 | `ds[0x00280010]` | 0.31 |
| 2 | `ds.Rows` | 0.32 |
| 3 | `ds["Rows"]` | 0.33 |
| 4 | `ds.get_dataelement(0x00280010).to_long()` | 0.78 |
| 5 | `ds.get_dataelement(0x00280010).get_value()` | 0.78 |

### pydicom comparison (Rows, 0x0028,0010), 100k iterations

| Rank | Access pattern | us / call |
| --- | --- | --- |
| 1 | `pydicom get_item((0x0028,0x0010)).value` | 0.73 |
| 2 | `pydicom int(get_item((0x0028,0x0010)).value)` | 0.77 |
| 3 | `pydicom ds[(0x0028,0x0010)].value` | 0.82 |
| 4 | `pydicom int(ds[(0x0028,0x0010)].value)` | 0.91 |
| 5 | `pydicom ds.Rows` | 0.98 |

### Sequence path, 5,000 iterations

| Rank | Access pattern | us / call |
| --- | --- | --- |
| 1 | `ds["00540016.0.00181075"]` | 0.46 |
| 2 | `ds["RadiopharmaceuticalInformationSequence.0.RadionuclideHalfLife"]` | 0.68 |
| 3 | `ds.RadiopharmaceuticalInformationSequence[0].RadionuclideHalfLife` | 1.28 |
| 4 | `ds.get_dataelement(0x00540016).sequence[0].get_dataelement(0x00181075).to_long()` | 2.16 |
| 5 | `ds.get_dataelement(0x00540016).sequence[0].get_dataelement(0x00181075).get_value()` | 2.16 |

## v0.1.5 (nanobind) — average of 5 runs

### Scalars: Rows (0x0028,0010), 100k iterations each

| Rank | Access pattern | us / call |
| --- | --- | --- |
| 1 | `ds[0x00280010]` | 0.11 |
| 2 | `ds["Rows"]` | 0.14 |
| 3 | `ds.Rows` | 0.15 |
| 4 | `ds.get_dataelement(0x00280010).get_value()` | 0.16 |
| 5 | `ds.get_dataelement(0x00280010).to_long()` | 0.20 |

### pydicom comparison (Rows, 0x0028,0x0010), 100k iterations

| Rank | Access pattern | us / call |
| --- | --- | --- |
| 1 | `pydicom int(get_item((0x0028,0x0010)).value)` | 0.78 |
| 2 | `pydicom get_item((0x0028,0x0010)).value` | 0.81 |
| 3 | `pydicom ds[(0x0028,0x0010)].value` | 0.83 |
| 4 | `pydicom int(ds[(0x0028,0x0010)].value)` | 0.90 |
| 5 | `pydicom ds.Rows` | 0.98 |

### Sequence path, 5,000 iterations

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
