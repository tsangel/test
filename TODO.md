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

Performance regression note:
- As of 2026-03-19, the large WG04 wheel-to-wheel `J2KR` slowdown was traced to the
  JPEG2000 auto decode thread policy introduced with split `worker_threads` /
  `codec_threads`.
- Fixed in `master`:
  - JPEG2000 `codec_threads=-1` no longer clamps to a fixed `8` threads.
  - Auto policy now uses hardware thread budget again for single-frame /
    single-worker decode paths, while still reducing codec threads when many
    outer workers are active.
  - Regression coverage added in `tests/decode_thread_policy_smoke.cpp`.
- Remaining follow-up:
  - rebuild the Windows wheel and rerun the WG04 benchmark to confirm `J2KR`
    returns near the pre-`0.1.30` range.

Deferred design decision:
- `decode_into` 색공간 출력 정책 정리
  - 옵션 A: 항상 RGB로 정규화해서 반환
  - 옵션 B: codestream/native 의미를 유지해서 반환(현재 동작)
  - TODO: C++ `decode_opts`에 `color_out`(e.g. `native`/`rgb`)를 둘지, Python `pixel_array`/`to_pil_image`와 정책을 어떻게 분리할지 결정

## Pre-release implementation backlog

Priority: Must
- [x] `DicomFile.to_pil_image()`에서 `PALETTE COLOR` 지원 추가.
  - Python regression covers classic palette, supplemental palette, and simple enhanced palette paths.
- [ ] `decode_into`/`to_array` 색공간 출력 정책 확정:
  - `native` 유지 vs `rgb` 정규화
  - C++ `DecodeOptions`/Python 바인딩의 옵션 이름/기본값 통일
- [x] `set_transfer_syntax`의 `encapsulated -> encapsulated` 트랜스코딩 지원.
  - 현재 구현은 codestream pass-through가 아니라 `decode-to-native + re-encode` 경로.

Priority: Should
- [ ] `set_transfer_syntax`의 `set_pixel_data` 경로에서 `PhotometricInterpretation` 지원 범위 확대
  - 예: `PALETTE COLOR`, `ARGB`, `CMYK`, `YBR_PARTIAL_420/422` 등 정책 결정 및 구현
- [ ] Windows wheel 재빌드 후 WG04 `J2KR` 벤치 재측정 및 문서/표 업데이트
  - `Restore JPEG2000 auto decode thread policy` 반영 결과 확인
- [ ] `tests/codec_cycle_realdata.cpp`의 known skip 원인 제거
  - `"unsupported PhotometricInterpretation"`
  - `"transfer syntax is not supported yet in set_pixel_data"`
- [x] JPEG2000 encode 경로의 `bits_allocated > 16` 지원 여부 결정 및 반영
  - 현재는 미지원으로 고정.
  - 제약은 `encode_target_policy.cpp`와 `docs/pixel_encode_constraints.md`에 반영.

Priority: Nice
- [ ] JPEGXL ON/OFF 빌드 조합을 CI에서 정기 검증하고 실패 시나리오를 문서화.

## String / charset follow-up

Completed in `master`
- [x] `to_utf8_view()` / `to_utf8_views()` 제거, owning 반환 API `to_utf8_string()` / `to_utf8_strings()`로 정리.
- [x] `from_utf8*` 계열 public API와 charset mutation/error policy 구현.
- [x] `SpecificCharacterSet (0008,0005)` 기반 decode/encode 구현
  - single-byte set
  - UTF-8 (`ISO_IR 192`)
  - ISO 2022 escape 시퀀스 기반 확장 세트
  - GBK / GB18030
- [x] multi-valued textual VR의 값 분리와 decode 경계 처리 정리.
- [x] structured PN API 추가
  - `PersonNameGroup`
  - `PersonName`
  - `DataElement.to_person_name()` / `from_person_name()`
- [x] C++/Python 테스트, stub, Python reference 문서 갱신.

Remaining follow-up
- [ ] `../sample/charsettests` 기반 회귀 테스트를 자동화해서 C++/Python CI에 편입.
- [ ] `PersonName::empty()` / `PersonNameGroup::empty()`의 의미를 문서화
  - explicit empty component/group(`^^^^`, trailing `=`)를 보존해도 `empty()==true`가 될 수 있음
- [ ] large dataset에서 `set_specific_charset(..., strict)` / transcoding 경로의 CPU, memory 회귀를 별도 벤치로 확인.
- [ ] release note에 API 변경점 명시
  - `to_utf8_view*` 제거
  - `to_utf8_string*` / `from_utf8*` / `PersonName*` 추가
- [ ] charset 정책 문서 보강
  - partial done:
    - raw string API와 UTF-8 API의 차이
    - invalid sequence error policy(`strict`, replacement, replace/remove 등) 정리
  - remaining:
    - `SpecificCharacterSet` 상속/override 규칙을 사용자 문서에 명시
