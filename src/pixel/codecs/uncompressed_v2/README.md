# pixel/codecs/uncompressed_v2

`pixel/codecs/uncompressed_v2`는 plugin ABI 경계를 거치지 않는 direct uncompressed 경로를 둔다.

현재 구현된 공개 진입점:

- `pixel::core_v2::encode_uncompressed_frame(...)`
- `pixel::core_v2::decode_uncompressed_frame(...)`
- `pixel::core_v2::copy_last_error_detail(...)`
- `pixel::core_v2::supported_profile_flags()`

내부 C++ 지원 구현:

- `support.hpp/.cpp`
  - `ErrorState` detail copy/fail helpers
  - dtype/stride validation helpers shared by uncompressed encode/decode
- `decode.cpp`
- `encode.cpp`

대상 profile:

- `PIXEL_CODEC_PROFILE_NATIVE_UNCOMPRESSED_V2`
- `PIXEL_CODEC_PROFILE_ENCAPSULATED_UNCOMPRESSED_V2`

위 두 profile은 plugin API 테이블(`pixel_*_plugin_api_v2`)을 사용하지 않고 direct 함수로 바로 진입한다.
runtime registry 초기화 시(`init_builtin_registry_v2`) core profile 슬롯은
`register_core_routes(...)`로 함께 채워진다.

동작/제약:

- 입력/출력은 `pixel_decoder_request_v2`, `pixel_encoder_request_v2` ABI 구조체를 그대로 사용한다.
- decode 경로는 `value_transform`을 지원한다.
  - `NONE`: 기존 동작(`source_dtype == dst_dtype`)
  - `RESCALE`/`MODALITY_LUT`: `samples_per_pixel == 1` + float destination(`F32/F64`) 조건에서 지원
- encode 경로는 버퍼 부족 시 `PIXEL_CODEC_ERR_OUTPUT_TOO_SMALL`를 반환하고 `output.encoded_size`에 필요한 크기를 기록한다.
- 실패 상세 문자열은 `ErrorState`에 저장되며 `copy_last_error_detail`로 복사 조회한다.
- registry 통합을 위해 core 지원 프로필은 `supported_profile_flags()`(64-bit)로 노출한다.

목적/의도:

- uncompressed 처리에서 불필요한 ABI bridge 오버헤드 제거
- profile별 책임 경계 명확화

테스트:

- `tests/pixel_core_uncompressed_smoke.cpp`
