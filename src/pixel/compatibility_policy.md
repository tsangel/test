# Pixel_ Compatibility Policy

`src/pixel`는 기존 구현과 **호환성을 고려하지 않는 신규 라인**이다.

## 1. Scope

- 대상: `src/pixel` 하위의 ABI/runtime/plugin 문서 및 구현
- 버전: v2 (clean-slate)

## 2. Hard Rules

1. 기존 `src/pixel`과 source/binary 호환성을 목표로 하지 않는다.
2. 기존 ABI(`include/pixel_*_plugin_abi.h`, `dicomsdl_*_v1`)를 재사용하지 않는다.
3. `pixel_` 안에 v1 shim/adapter/alias 심볼을 만들지 않는다.
4. 신규 인터페이스는 `pixel_*_v2` 계열 이름으로 정의한다.
5. 기존 플러그인은 재작성 대상으로 본다.
6. plugin ABI 경계에는 `transfer_syntax_code`를 전달하지 않고 `codec_profile_code`만 전달한다.
7. plugin 식별자는 v2 ABI 선택 기준이나 public surface에 포함하지 않는다.
8. codec 지원 정보는 `supported_profile_flags`(64-bit bitmask)로 전달한다.

## 3. Error Policy

1. 에러 분류는 최소 `error_code` 집합으로 유지한다.
2. 상세 정보는 `detail` 문자열(`stage=...;reason=...`)에 기록한다.
3. 문자열 파싱은 디버깅/로그 목적이며, 분기 로직은 `error_code`만 사용한다.
4. `configure/decode/encode` 콜백은 `0/1` 대신 `error_code`를 직접 반환한다.
5. 상세 문자열 조회는 `copy_last_error_detail(ctx, out, cap)` 콜백으로 표준화한다.

## 4. Context Policy

1. 모든 `ctx`는 단일 스레드 전용(thread-affine)으로 사용한다.
2. `*_create`는 컨텍스트 메모리 할당 + 기본값 초기화만 수행한다.
3. 옵션 파싱/백엔드 준비/실패 가능 로직은 `*_configure`로 이동한다.
4. `create` 이후 실패 상세는 `ctx`의 `last_error_detail` 문자열에 저장한다.
5. `ctx`에는 `last_error_detail`만 저장하고, 에러 코드는 함수 반환값으로 전달한다.
6. `create` 실패는 `ctx`가 없으므로 `last_error_detail` 저장 대상이 없다(일반적으로 null 반환).

## 5. ABI File Status

1. `src/pixel/abi`는 `pixel_*_v2` 헤더만 유지한다.
2. 신규 구현/문서는 `pixel_*_v2` 타입/심볼만 기준으로 작성한다.
3. `pixel_` 범위에서 `*_v1.h` 재도입은 허용하지 않는다.

## 6. Codec Placement Policy

1. `NATIVE_UNCOMPRESSED`와 `ENCAPSULATED_UNCOMPRESSED`는 plugin 경계를 통과하지 않고 `pixel_` core 경로에서 직접 처리한다.
2. `RLE_LOSSLESS`는 `src/pixel/codecs/rle_v2`에 builtin codec으로 둔다.
3. JPEG/JPEG-LS/JPEG2000/HTJ2K/JPEG-XL 계열의 코드 위치는 `src/pixel/codecs/*_v2`를 기준으로 한다.
4. codec family별로 builtin registration과 loadable export를 같은 디렉터리에서 병행 구현할 수 있다.
5. 구조 통일을 위해 코드 루트는 모두 `src/pixel` 아래에 유지한다.

## 7. Registry Policy (v2)

1. registry는 `profile_code` 기준 고정 32슬롯 테이블(`0..31`)을 사용한다.
2. 각 슬롯은 decoder/encoder 엔트리를 함께 보관한다.
3. 슬롯 채움은 plugin의 `supported_profile_flags`(64-bit)를 decode해서 수행한다.
4. 동일 슬롯 충돌은 decoder/encoder 각각 `last registration wins` 정책으로 처리한다.
5. 선택 기준으로 `priority`나 별도 plugin 식별자를 사용하지 않는다.

## 8. Runtime Bootstrap Policy

1. `initialize_registry_v2`는 같은 `PluginRegistryRuntimeV2` 인스턴스에서 `std::call_once`로 1회만 실제 초기화를 수행한다.
2. `shutdown_registry_v2` 호출 이후에는 동일 인스턴스 재초기화를 허용하지 않는다.
3. `init_builtin_registry_v2`는 core + static-linked plugin들을 등록한다.
4. 기본 등록 순서는 `RLE -> JPEG -> JPEG-LS -> HTJ2K -> OpenJPEG -> JPEG-XL`이다.
5. 같은 슬롯 충돌 시 후등록이 덮어쓰므로, 현재 순서에서는 HTJ2K 관련 decode 슬롯에서 OpenJPEG가 최종 등록된다.

## 9. Overhead Policy

1. uncompressed 경로는 copy/pack 중심 경량 처리이므로 plugin ABI request 조립/검증/간접호출 오버헤드를 피하기 위해 core 직통을 기본으로 한다.
2. `RLE_LOSSLESS`는 static 링크로 유지하여 동적 로딩(dlopen/LoadLibrary) 오버헤드는 없애고, API 테이블 기반 디스패치 비용만 허용한다.
3. 압축 코덱(shared plugin)의 호출 오버헤드는 codec 처리 비용 대비 미미하므로 분리/확장성을 우선한다.
