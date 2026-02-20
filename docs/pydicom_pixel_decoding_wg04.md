# pydicom Pixel Decoding on WG04 (Benchmark Notes)

This note summarizes how to benchmark WG04 pixel decoding with `pydicom`,
and how to compare it with `dicomsdl` using the same driver script.

## Benchmark script

- Script: `benchmarks/python/benchmark_wg04_pixel_decode.py`
- WG04 root selection order:
  - CLI argument `root` (if provided)
  - `DICOMSDL_WG04_IMAGES_BASE` environment variable
  - default: `/Users/tsangel/workspace.dev/sample/nema/WG04/IMAGES`

Supported codec directories:

- `REF`
- `RLE`
- `J2KR`
- `J2KI`
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
  --json build/wg04_pixel_decode_compare_r3.json
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
| REF  | 0.428 | 0.486 | 1.14 | 1.00 |
| RLE  | 4.925 | 52.145 | 10.59 | 2.25 |
| J2KR | 42.887 | 72.867 | 1.70 | 3.79 |
| J2KI | 19.151 | 48.806 | 2.55 | 32.03 |
| JLSL | 38.056 | 57.333 | 1.51 | 4.13 |
| JLSN | 32.938 | 54.069 | 1.64 | 9.72 |
| JPLL | 15.149 | 45.785 | 3.02 | 3.12 |
| JPLY | 7.109 | 33.830 | 4.76 | 29.17 |
| TOTAL | 19.680 | 45.487 | 2.31 | 10.03 |

`dcm/pyd x` means `pydicom_ms_per_decode / dicomsdl_ms_per_decode`.
Values greater than `1.0` indicate `dicomsdl` is faster for that codec.
`CR(ref/x)` means case-matched average `size(REF_case) / size(codec_case)`.

## Notes

- During `pydicom` JPEG runs, decoder warnings such as
  `Unsupported JPEG data precision 12` and
  `Invalid SOS parameters for sequential JPEG`
  may appear on stderr. In this benchmark run, decoding still completed and
  timing data was collected.
- `--scaled` is intentionally restricted to `dicomsdl` mode in this script.
  Cross-backend comparisons should use `scaled=False`.
- Full numeric output is stored in
  `build/wg04_pixel_decode_compare_r3.json` from the command above.
