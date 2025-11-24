# Python DataSet access quick notes

Benchmarks (Apple M3 / Python 3.12)

## Scalars: Rows (0x0028,0010) — average of 3 runs, 100k iterations each

| Rank | Access pattern | us / call |
| --- | --- | --- |
| 1 | `ds.Rows` | 0.25 |
| 2 | `ds[0x00280010]` | 0.28 |
| 3 | `ds["Rows"]` | 0.32 |
| 4 | `ds.get_dataelement(0x00280010).to_long()` | 0.98 |
| 5 | `ds.get_dataelement(0x00280010).get_value()` | 1.00 |

Note: `ds.Rows` is a bit faster than `ds[0x00280010]` because `__getattr__` does a direct keyword→Tag conversion with minimal type checks, while `__getitem__` branches through `py::isinstance` for Tag/int/str before resolving the element.

## Sequence path (5,000 iterations, average of 3 runs, sample/…fb.dcm)

| Rank | Access pattern | us / call |
| --- | --- | --- |
| 1 | `ds["00540016.0.00181075"]` | 0.48 |
| 2 | `ds["RadiopharmaceuticalInformationSequence.0.RadionuclideHalfLife"]` | 0.73 |
| 3 | `ds.RadiopharmaceuticalInformationSequence[0].RadionuclideHalfLife` | 1.59 |
| 4 | `ds.get_dataelement(0x00540016).sequence[0].get_dataelement(0x00181075).get_value()` | 2.83 |
| 5 | `ds.get_dataelement(0x00540016).sequence[0].get_dataelement(0x00181075).to_long()` | 2.84 |

Takeaway: path-based indexing is fastest, but differences are microsecond-level—prefer readability unless you are in a tight loop.
Note: For sequences, single-shot path parsing (#1–2) avoids extra object hops (`sequence` attribute, repeated `get_dataelement`), so it wins by microseconds; in deep loops, favor fewer chained calls.
