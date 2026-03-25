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
  - this is applied once during dicomsdl startup, before the first pixel decode/encode
  - mid-run backend fallback is not supported

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
  --dicomsdl-htj2k-decoder openjpeg \
  --warmup 1 \
  --repeat 3 \
  --json build/wg04_pixel_decode_compare_r3_htj2k.json
```

Useful command variants:

```bash
# Compare dicomsdl and pydicom
python benchmarks/python/benchmark_wg04_pixel_decode.py --backend both --warmup 1 --repeat 5

# dicomsdl with preallocated output reuse
python benchmarks/python/benchmark_wg04_pixel_decode.py --backend dicomsdl --reuse-output --repeat 10

# pydicom with output reuse
python benchmarks/python/benchmark_wg04_pixel_decode.py --backend pydicom --reuse-output-pydicom --repeat 10

# Limit the run to selected codecs
python benchmarks/python/benchmark_wg04_pixel_decode.py --codec JLSL --codec JLSN --repeat 10
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

기본 설정에서 `decode_into`는 `worker_threads=-1`, `codec_threads=-1`을 사용합니다.

## Measurement method

- Datasets are preloaded once per backend and codec before timed runs.
- `dicomsdl` path:
  - when `--dicomsdl-htj2k-decoder` is provided, the script configures the HTJ2K
    backend immediately after importing `dicomsdl`, before any decode work starts
  - default decode call: `df.to_array(frame=-1)`
  - for `J2KR`/`J2KI` in the base table, the driver forces
    `df.decode_into(out, frame=-1, worker_threads=-1, codec_threads=-1)`
  - with `--reuse-output`:
    `df.decode_into(out, frame=-1, worker_threads=-1, codec_threads=-1)`
  - in this benchmark driver, `decode_into` uses `worker_threads=-1`,
    `codec_threads=-1` by default.
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

## Snapshot result (`--warmup 1 --repeat 3`, HTJ2K compare target: dicomsdl `openjpeg`)

| Codec | dicomsdl ms/decode | pydicom ms/decode | dcm/pyd x | CR(ref/x) |
| --- | ---: | ---: | ---: | ---: |
| REF | 0.467 | 0.648 | 1.39 | 1.00 |
| RLE | 5.049 | 47.090 | 9.33 | 2.25 |
| J2KR | 40.780 | 74.590 | 1.83 | 3.79 |
| J2KI | 17.858 | 48.745 | 2.73 | 32.03 |
| JLSL | 36.500 | 57.317 | 1.57 | 4.13 |
| JLSN | 32.795 | 53.521 | 1.63 | 9.72 |
| JPLL | 14.617 | 45.091 | 3.08 | 3.12 |
| TOTAL | 19.949 | 45.786 | 2.30 | 8.42 |

`dcm/pyd x` means `pydicom_ms_per_decode / dicomsdl_ms_per_decode`.
Values greater than `1.0` indicate `dicomsdl` is faster for that codec.
`CR(ref/x)` means case-matched average `size(REF_case) / size(codec_case)`.
`TOTAL` excludes `htj2k*` rows (`htj2kll`, `htj2kly`).

## HTJ2K backend snapshot (separate table, main-table aligned)

Condition:
- `--backend dicomsdl --dicomsdl-mode to_array --warmup 1 --repeat 3`
- same decode mode/iteration settings as the main table; only `--dicomsdl-htj2k-decoder` changes

HTJ2K cross-backend comparison target in this note:
- `htj2kll (openjpeg)` vs `pydicom`
- `htj2kly (openjpeg)` vs `pydicom`

| Variant | dicomsdl ms/decode | pydicom ms/decode | dcm/pyd x | CR(ref/x) |
| --- | ---: | ---: | ---: | ---: |
| htj2kll (openjpeg) | 12.839 | 40.286 | 3.14 | 3.39 |
| htj2kly (openjpeg) | 14.681 | 37.233 | 2.54 | 39.24 |

| Variant | dicomsdl ms/decode | CR(ref/x) |
| --- | ---: | ---: |
| htj2kll (openjpeg) | 12.839 | 3.39 |
| htj2kll (openjph) | 22.163 | 3.39 |
| htj2kly (openjpeg) | 14.681 | 39.24 |
| htj2kly (openjph) | 19.330 | 39.24 |

## Notes

- During `pydicom` JPEG runs, decoder warnings such as
  `Unsupported JPEG data precision 12` and
  `Invalid SOS parameters for sequential JPEG`
  may appear on stderr. In this benchmark run, decoding still completed and
  timing data was collected.
- HTJ2K (`1.2.840.10008.1.2.4.201`, `1.2.840.10008.1.2.4.203`) decoding in
  `pydicom` depends on plugin availability/runtime compatibility. If decode fails
  in a run, HTJ2K rows are reported as `pydicom=n/a`.
- In this benchmark flow, dataset pixel metadata is assumed immutable after
  preload. If pixel-affecting fields are changed, regenerate pixel metadata and
  reallocate output buffers before subsequent `decode_into` calls.
- Full numeric output is stored in
  `build/wg04_pixel_decode_compare_r3_htj2k.json` from the command above.
- HTJ2K backend-only JSON snapshots:
  - `build/wg04_htj2k_openjpeg_r3_toarray_postopt.json`
  - `build/wg04_htj2k_openjph_r3_toarray_postopt.json`
