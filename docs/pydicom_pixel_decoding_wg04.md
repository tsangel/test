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

## Measurement method

- Datasets are preloaded once per backend and codec before timed runs.
- `dicomsdl` path:
  - decode call: `ds.to_array(frame=-1, scaled=False)`
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

## Snapshot result (`--warmup 1 --repeat 3`)

| Codec | dicomsdl ms/decode | pydicom ms/decode | dcm/pyd x |
| --- | ---: | ---: | ---: |
| REF  | 0.605 | 0.391 | 0.65 |
| RLE  | 4.982 | 53.148 | 10.67 |
| J2KR | 254.430 | 72.186 | 0.28 |
| J2KI | 62.363 | 47.267 | 0.76 |
| JLSL | 35.631 | 55.878 | 1.57 |
| JLSN | 32.300 | 53.079 | 1.64 |
| JPLL | 14.383 | 43.757 | 3.04 |
| JPLY | 6.853 | 31.323 | 4.57 |
| TOTAL | 58.808 | 44.638 | 0.76 |

`dcm/pyd x` means `pydicom_ms_per_decode / dicomsdl_ms_per_decode`.
Values greater than `1.0` indicate `dicomsdl` is faster for that codec.

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
