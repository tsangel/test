# pydicom Pixel Decoding on WG04 (Benchmark Notes)

This note summarizes how to benchmark WG04 pixel decoding with `pydicom`,
and how to compare it with `dicomsdl` using the same driver script.

## Benchmark script

- Script: `benchmarks/python/benchmark_wg04_pixel_decode.py`
- WG04 root selection order:
  - CLI argument `root` (if provided)
  - `DICOMSDL_WG04_IMAGES_BASE` environment variable
  - default: `/Users/tsangel/workspace.dev/sample/nema/WG04/IMAGES`
- HTJ2K backend override for dicomsdl:
  - `--dicomsdl-htj2k-decoder auto|openjph|openjpeg`

Supported codec directories:

- `REF`
- `RLE`
- `J2KR`
- `J2KI`
- `htj2kll`
- `htj2kly`
- `JLSL`
- `JLSN`
- `JPLL`
- `JPLY`

## Repro command

```bash
python benchmarks/python/benchmark_wg04_pixel_decode.py \
  --backend both \
  --warmup 1 \
  --repeat 3 \
  --json build/wg04_pixel_decode_compare_r3_htj2k.json
```

`pydicom`에서 출력 버퍼 재사용 모드:

```bash
python benchmarks/python/benchmark_wg04_pixel_decode.py \
  --backend pydicom \
  --reuse-output-pydicom \
  --warmup 1 \
  --repeat 10
```

`dicomsdl`만 대상으로 preallocated output 재사용(`decode_into`) 경로를 측정하려면:

```bash
python benchmarks/python/benchmark_wg04_pixel_decode.py \
  --backend dicomsdl \
  --reuse-output \
  --warmup 1 \
  --repeat 10
```

기본 설정에서 `decode_into` thread hint는 `threads=-1`(all CPUs)입니다.

## Measurement method

- Datasets are preloaded once per backend and codec before timed runs.
- `dicomsdl` path:
  - default decode call: `ds.to_array(frame=-1, scaled=False)`
  - for `J2KR`/`J2KI` in the base table, the driver forces
    `ds.decode_into(out, frame=-1, scaled=False, threads=-1)` to apply multi-CPU decoding
  - with `--reuse-output`: `ds.decode_into(out, frame=-1, scaled=False, threads=-1)`
  - in this benchmark driver, `decode_into` uses `threads=-1` by default
    (JPEG 2000: auto/all CPUs).
- `pydicom` path:
  - decode call: `ds.pixel_array`
  - before each timed decode, `_pixel_id` is reset to force re-decode and avoid
    returning cached pixel arrays.
  - with `--reuse-output-pydicom`:
    - uncompressed: `numpy_handler.get_pixeldata(read_only=True)` +
      `reshape_pixel_array` + `np.copyto(out)`
    - compressed: `pixel_array` decode + `np.copyto(out)` fallback
- Metrics per codec:
  - `ms/decode`
  - `MPix/s`
  - `MiB/s`
  - `CR(ref/x)`: case-matched 평균 `REF 파일크기 / codec 파일크기`

## Snapshot result (`--warmup 1 --repeat 3`, dicomsdl default threads `-1`)

| Codec | dicomsdl ms/decode | pydicom ms/decode | dcm/pyd x | CR(ref/x) |
| --- | ---: | ---: | ---: | ---: |
| REF  | 0.325 | 0.411 | 1.27 | 1.00 |
| RLE  | 4.783 | 51.387 | 10.74 | 2.25 |
| J2KR | 39.542 | 70.990 | 1.80 | 3.79 |
| J2KI | 17.959 | 49.152 | 2.74 | 32.03 |
| JLSL | 36.235 | 56.174 | 1.55 | 4.13 |
| JLSN | 32.412 | 53.363 | 1.65 | 9.72 |
| JPLL | 14.484 | 44.534 | 3.07 | 3.12 |
| JPLY | 6.972 | 31.860 | 4.57 | 29.17 |
| TOTAL | 18.625 | 44.654 | 2.40 | 10.03 |

`dcm/pyd x` means `pydicom_ms_per_decode / dicomsdl_ms_per_decode`.
Values greater than `1.0` indicate `dicomsdl` is faster for that codec.
`CR(ref/x)` means case-matched average `size(REF_case) / size(codec_case)`.
`TOTAL` excludes `htj2k*` rows (`htj2kll`, `htj2kly`).

## HTJ2K backend snapshot (separate table, main-table aligned)

Condition:
- `--backend dicomsdl --dicomsdl-mode to_array --warmup 1 --repeat 3`
- same decode mode/iteration settings as the main table; only `--dicomsdl-htj2k-decoder` changes

| Variant | dicomsdl ms/decode | CR(ref/x) |
| --- | ---: | ---: |
| htj2kll (openjpeg) | 13.039 | 3.39 |
| htj2kll (openjph)  | 23.961 | 3.39 |
| htj2kly (openjpeg) | 15.599 | 39.24 |
| htj2kly (openjph)  | 21.610 | 39.24 |

## Notes

- During `pydicom` JPEG runs, decoder warnings such as
  `Unsupported JPEG data precision 12` and
  `Invalid SOS parameters for sequential JPEG`
  may appear on stderr. In this benchmark run, decoding still completed and
  timing data was collected.
- In this environment, `pydicom` did not decode HTJ2K transfer syntaxes
  (`1.2.840.10008.1.2.4.201`, `1.2.840.10008.1.2.4.203`), so those rows are
  reported in the separate HTJ2K table as `dicomsdl`-only (`pydicom=n/a`).
- `--scaled` is intentionally restricted to `dicomsdl` mode in this script.
  Cross-backend comparisons should use `scaled=False`.
- Full numeric output is stored in
  `build/wg04_pixel_decode_compare_r3_htj2k.json` from the command above.
- HTJ2K backend-only JSON snapshots:
  - `build/wg04_htj2k_openjpeg_r3_toarray_postopt.json`
  - `build/wg04_htj2k_openjph_r3_toarray_postopt.json`
