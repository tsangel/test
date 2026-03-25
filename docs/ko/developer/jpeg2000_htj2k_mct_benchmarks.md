# JPEG2000/HTJ2K MCT Benchmarks (Auto Encoder Threads)

```{note}
이 페이지 본문은 아직 영어 원문입니다. 필요하면 영문 페이지를 기준으로 읽어 주세요.
```

## Scope
- Measured date: 2026-02-24
- Input files:
  - `../sample/nema/WG04/IMAGES/REF/US1_UNC`
  - `../sample/nema/WG04/IMAGES/REF/VL1_UNC`
  - `../sample/nema/WG04/IMAGES/REF/VL2_UNC`
  - `../sample/nema/WG04/IMAGES/REF/VL3_UNC`
  - `../sample/nema/WG04/IMAGES/REF/VL4_UNC`
  - `../sample/nema/WG04/IMAGES/REF/VL5_UNC`
  - `../sample/nema/WG04/IMAGES/REF/VL6_UNC`
- Codec configurations:
  - `J2K_MCT_ON`: `JPEG2000Lossless`, `color_transform=True`
  - `J2K_MCT_OFF`: `JPEG2000Lossless`, `color_transform=False`
  - `HTJ2K_MCT_ON`: `HTJ2KLossless`, `color_transform=True`
  - `HTJ2K_MCT_OFF`: `HTJ2KLossless`, `color_transform=False`
- Thread policy:
  - Encoder default is `threads=-1` (auto CPU), matching decode-style semantics.
  - `threads=0` keeps library default, `threads>0` sets explicit thread count.
- Benchmark policy:
  - Warm-up first run is discarded.
  - Measured repetitions per case: 4
  - Encode benchmark: `ExplicitVRLittleEndian -> target transfer syntax`
  - Decode benchmark: `target transfer syntax -> ExplicitVRLittleEndian`

## Aggregate Summary

`raw_total = 44,558,496 bytes`

| Config | enc_total (bytes) | Compression Ratio (raw/enc) | Encode ms (sum of per-file means) | Decode ms (sum of per-file means) | Encode MB/s | Decode MB/s |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| J2K_MCT_ON | 22,632,126 | 1.968816 | 471.772 | 404.753 | 94.449 | 110.088 |
| J2K_MCT_OFF | 19,289,030 | 2.310043 | 413.996 | 365.281 | 107.630 | 121.984 |
| HTJ2K_MCT_ON | 24,075,629 | 1.850772 | 487.586 | 266.130 | 91.386 | 167.431 |
| HTJ2K_MCT_OFF | 20,608,595 | 2.162132 | 460.366 | 247.524 | 96.789 | 180.017 |

## Per-File Detailed Results

| File | raw_bytes | Config | enc_bytes | Ratio | Encode ms | Decode ms | Encode MB/s | Decode MB/s |
| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| US1_UNC | 921,600 | J2K_MCT_ON | 152,322 | 6.050341 | 6.886 | 6.343 | 133.842 | 145.284 |
| US1_UNC | 921,600 | J2K_MCT_OFF | 334,392 | 2.756047 | 10.257 | 9.297 | 89.849 | 99.130 |
| US1_UNC | 921,600 | HTJ2K_MCT_ON | 167,535 | 5.500940 | 4.988 | 3.212 | 184.766 | 286.914 |
| US1_UNC | 921,600 | HTJ2K_MCT_OFF | 365,778 | 2.519561 | 7.966 | 4.622 | 115.689 | 199.396 |
| VL1_UNC | 1,102,248 | J2K_MCT_ON | 295,849 | 3.725711 | 8.796 | 7.851 | 125.316 | 140.405 |
| VL1_UNC | 1,102,248 | J2K_MCT_OFF | 279,473 | 3.944023 | 8.682 | 8.382 | 126.963 | 131.503 |
| VL1_UNC | 1,102,248 | HTJ2K_MCT_ON | 316,995 | 3.477178 | 8.114 | 4.929 | 135.847 | 223.609 |
| VL1_UNC | 1,102,248 | HTJ2K_MCT_OFF | 304,988 | 3.614070 | 7.508 | 4.560 | 146.819 | 241.738 |
| VL2_UNC | 1,102,248 | J2K_MCT_ON | 307,526 | 3.584243 | 8.874 | 8.077 | 124.216 | 136.467 |
| VL2_UNC | 1,102,248 | J2K_MCT_OFF | 322,847 | 3.414150 | 9.396 | 9.694 | 117.311 | 113.703 |
| VL2_UNC | 1,102,248 | HTJ2K_MCT_ON | 329,259 | 3.347662 | 8.492 | 5.201 | 129.795 | 211.929 |
| VL2_UNC | 1,102,248 | HTJ2K_MCT_OFF | 349,519 | 3.153614 | 8.211 | 4.942 | 134.238 | 223.055 |
| VL3_UNC | 1,102,248 | J2K_MCT_ON | 302,960 | 3.638262 | 8.879 | 8.048 | 124.146 | 136.954 |
| VL3_UNC | 1,102,248 | J2K_MCT_OFF | 291,242 | 3.784646 | 9.003 | 8.687 | 122.425 | 126.882 |
| VL3_UNC | 1,102,248 | HTJ2K_MCT_ON | 329,301 | 3.347236 | 9.014 | 5.371 | 122.283 | 205.210 |
| VL3_UNC | 1,102,248 | HTJ2K_MCT_OFF | 323,954 | 3.402483 | 8.870 | 5.212 | 124.267 | 211.497 |
| VL4_UNC | 12,474,504 | J2K_MCT_ON | 5,852,413 | 2.131515 | 128.925 | 103.825 | 96.758 | 120.150 |
| VL4_UNC | 12,474,504 | J2K_MCT_OFF | 4,341,517 | 2.873305 | 92.405 | 82.506 | 134.997 | 151.196 |
| VL4_UNC | 12,474,504 | HTJ2K_MCT_ON | 6,215,167 | 2.007107 | 120.401 | 67.442 | 103.608 | 184.967 |
| VL4_UNC | 12,474,504 | HTJ2K_MCT_OFF | 4,602,504 | 2.710373 | 94.375 | 54.662 | 132.181 | 228.210 |
| VL5_UNC | 26,753,400 | J2K_MCT_ON | 15,109,819 | 1.770597 | 295.839 | 258.752 | 90.432 | 103.394 |
| VL5_UNC | 26,753,400 | J2K_MCT_OFF | 13,047,541 | 2.050455 | 270.074 | 233.356 | 99.060 | 114.646 |
| VL5_UNC | 26,753,400 | HTJ2K_MCT_ON | 16,034,994 | 1.668438 | 323.513 | 172.899 | 82.696 | 154.735 |
| VL5_UNC | 26,753,400 | HTJ2K_MCT_OFF | 13,915,978 | 1.922495 | 320.767 | 166.591 | 83.405 | 160.594 |
| VL6_UNC | 1,102,248 | J2K_MCT_ON | 611,237 | 1.803307 | 13.574 | 11.858 | 81.205 | 92.957 |
| VL6_UNC | 1,102,248 | J2K_MCT_OFF | 672,018 | 1.640206 | 14.178 | 13.359 | 77.742 | 82.507 |
| VL6_UNC | 1,102,248 | HTJ2K_MCT_ON | 682,378 | 1.615304 | 13.064 | 7.075 | 84.371 | 155.785 |
| VL6_UNC | 1,102,248 | HTJ2K_MCT_OFF | 745,874 | 1.477794 | 12.670 | 6.936 | 86.997 | 158.927 |

## Notes
- Compression ratio is determined by codestream output and does not change with thread count.
- The large J2K encode speed change came from enabling auto-threading by default.
- HTJ2K path in current OpenJPH integration does not explicitly consume `Htj2kOptions.threads` for encode yet.

## HTJ2K Decode Backend Comparison (Forced OpenJPH/OpenJPEG)

This section adds decode-backend-forced results for HTJ2K (`openjph` vs `openjpeg`), while keeping the same encode settings and MCT on/off combinations.

- Measured date: 2026-02-24
- Decode benchmark method: `set_htj2k_decoder_backend(...)` + `to_array(frame=-1)`
- Backend selection is process-initial only in the current runtime design; it must be
  configured before the first pixel decode/encode.
- Warm-up discarded: 1 run
- Measured repetitions: 4 runs
- Encoder side remains `threads=-1` (auto CPU)

### Aggregate Summary (Backend Forced)

`raw_total = 44,558,496 bytes`

| Config | enc_total (bytes) | Compression Ratio (raw/enc) | Encode ms (sum of per-file means) | Decode ms (sum of per-file means) | Encode MB/s | Decode MB/s |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| J2K_MCT_ON | 22,632,126 | 1.968816 | 460.272 | 394.426 | 96.809 | 112.970 |
| J2K_MCT_OFF | 19,289,030 | 2.310043 | 429.149 | 391.718 | 103.830 | 113.752 |
| HTJ2K_MCT_ON_OPENJPH | 24,075,629 | 1.850772 | 492.993 | 269.307 | 90.384 | 165.456 |
| HTJ2K_MCT_ON_OPENJPEG | 24,075,629 | 1.850772 | 484.723 | 127.156 | 91.926 | 350.424 |
| HTJ2K_MCT_OFF_OPENJPH | 20,608,595 | 2.162132 | 464.897 | 247.447 | 95.846 | 180.073 |
| HTJ2K_MCT_OFF_OPENJPEG | 20,608,595 | 2.162132 | 460.662 | 136.669 | 96.727 | 326.033 |

### Per-File Decode Detail (HTJ2K Only)

| File | Mode | openjph Decode ms | openjpeg Decode ms | openjph Decode MB/s | openjpeg Decode MB/s |
| --- | --- | ---: | ---: | ---: | ---: |
| US1_UNC | MCT ON | 3.105 | 3.767 | 296.771 | 244.650 |
| US1_UNC | MCT OFF | 4.402 | 4.823 | 209.360 | 191.104 |
| VL1_UNC | MCT ON | 4.784 | 4.305 | 230.404 | 256.044 |
| VL1_UNC | MCT OFF | 4.612 | 4.868 | 239.006 | 226.423 |
| VL2_UNC | MCT ON | 5.370 | 4.356 | 205.261 | 253.025 |
| VL2_UNC | MCT OFF | 4.897 | 4.936 | 225.088 | 223.315 |
| VL3_UNC | MCT ON | 5.372 | 4.411 | 205.189 | 249.877 |
| VL3_UNC | MCT OFF | 5.160 | 4.619 | 213.607 | 238.655 |
| VL4_UNC | MCT ON | 67.523 | 31.107 | 184.744 | 401.014 |
| VL4_UNC | MCT OFF | 55.323 | 35.147 | 225.486 | 354.924 |
| VL5_UNC | MCT ON | 176.081 | 74.645 | 151.938 | 358.407 |
| VL5_UNC | MCT OFF | 166.018 | 76.856 | 161.148 | 348.100 |
| VL6_UNC | MCT ON | 7.071 | 4.564 | 155.876 | 241.517 |
| VL6_UNC | MCT OFF | 7.035 | 5.421 | 156.678 | 203.330 |

### Observations

- With this measurement method, HTJ2K decode was faster with forced `openjpeg` on most larger VL files.
- Aggregate decode speed:
  - MCT ON: `openjpeg` was about `2.12x` faster than `openjph` (`269.307 ms -> 127.156 ms`).
  - MCT OFF: `openjpeg` was about `1.81x` faster than `openjph` (`247.447 ms -> 136.669 ms`).
