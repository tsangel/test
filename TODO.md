# TODO

| SamplesPerPixel | bytes_per_sample | PhotometricInterpretation | Rescale/LUT apply? | Priority | Raw | RLE | JPEG | JPEG2K | JPEG-LS |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | 1 | MONOCHROME1/2 | apply | Must | 🟢 | 🟢 | 🟢 | 🟢 | 🟢 |
| 1 | 2 | MONOCHROME1/2 | apply | Must | 🟢 | 🟢 | 🟢 | 🟢 | 🟢 |
| 1 | 1 | PALETTE COLOR | no (palette LUT only) | Must | 🟢 | 🟢 | 🟡 | 🟡 | 🟡 |
| 1 | 2 | PALETTE COLOR (16-bit) | no | Should | 🟢 | 🟢 | 🟡 | 🟡 | 🟡 |
| 3 | 1 | RGB / YBR_FULL / YBR_PARTIAL / YBR_ICT / YBR_RCT | no | Must | 🟢 | 🟢 | 🟡 | 🟡 | 🟡 |
| 3 | 2 | RGB / YBR_RCT (16-bit) | no | Should | 🟢 | 🟢 | 🟡 | 🟡 | 🟡 |
| 4 | 1 | ARGB / CMYK | no | Should | 🟢 | 🟢 | 🟡 | 🟡 | 🟡 |
| 4 | 2 | ARGB / CMYK (16-bit) | no | Nice | 🟢 | 🟢 | 🟡 | 🟡 | 🟡 |
| 1 | 4 (float32) | MONO (Parametric Map, etc.) | generally no* | Should | 🟢 | 🟡 | ⚪ | ⚪ | ⚪ |
| 3/4 | 4 (float32) | Color float (rare) | no | Nice | 🟢 | 🟡 | ⚪ | ⚪ | ⚪ |

*float MONO datasets often already store real-world values; decide later if an opt-in rescale is needed.

Status legend:
- 🟢 완전구현
- 🟡 일부구현(제한 있거나/또는 검증 범위 부족)
- ⚪ 미구현

기준:
- `Rescale/LUT apply? = no` 항목은 `scaled=false` decode가 되면 🟢로 본다.
- `Rescale/LUT apply? = apply` 항목은 `scaled=true`에서 `SamplesPerPixel=1` 지원 시 🟢로 본다.

Current decoder limits:
- backend 구현: raw, rle, jpeg(libjpeg-turbo), jpeg2k(openjpeg), jpegls(charls)
- backend 미지원: jpegxl, video(mpeg2/h264/hevc), 기타 비이미지 TS
- 공통 layout transform: interleaved<->planar, planar->planar
- `SamplesPerPixel == 1/3/4` (현재)
- dtype/backend 제약:
  - raw/rle: `u8/s8/u16/s16/u32/s32/f32/f64`
  - jpeg(libjpeg-turbo): integral only, up to 16-bit
  - jpeg2k(openjpeg): integral only, up to 32-bit
  - jpegls(charls): integral only, up to 16-bit
- `scaled=true` 제한:
  - `SamplesPerPixel==1` + modality transform metadata(`ModalityLUT` 또는 `Rescale*`) 존재 시에만 유효
  - output dtype=`float32`
  - 처리 순서: `Modality LUT Sequence` 우선, 없으면 `Rescale Slope/Intercept`
- JPEG Extended 12-bit(Process 2/4, `1.2.840.10008.1.2.4.51`)은
  malformed `SOF1 + SOS(Se=0)` 헤더에 대한 compatibility patch를 포함.

Verification note:
- As of 2026-02-20, NEMA WG04 `IMAGES` regression:
  - REF smoke: 20/20
  - RLE vs REF: 20/20 exact
  - J2KR vs REF: 20/20 exact
  - J2KI vs REF: 20/20 (`MAE <= 55`)
  - JLSL vs REF: 13/13 exact
  - JLSN vs REF: 13/13 (`max abs error <= 10`)
  - JPLL vs REF: 13/13 exact
  - JPLY vs REF: 10/10 (`MAE <= 60`)
  - Total: 129/129 pass

Deferred design decision:
- `decode_into` 색공간 출력 정책 정리
  - 옵션 A: 항상 RGB로 정규화해서 반환
  - 옵션 B: codestream/native 의미를 유지해서 반환(현재 동작)
  - TODO: C++ `decode_opts`에 `color_out`(e.g. `native`/`rgb`)를 둘지, Python `pixel_array`/`to_pil_image`와 정책을 어떻게 분리할지 결정
