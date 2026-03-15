# UTF-8 API Charset 구현 상태

## 목적

DICOMSDL의 텍스트 API는 UTF-8을 기준으로 노출하고, 실제 DICOM 바이트는 dataset이 보유한
`(0008,0005) Specific Character Set` 선언에 맞춰 mutation 시점에 저장한다.

대상 API:

- `to_utf8_string()`
- `to_utf8_strings()`
- `from_utf8_view()`
- `from_utf8_views()`
- `DicomFile::set_declared_specific_charset()`
- `DicomFile::set_specific_charset()`

## 현재 구현 원칙

### 1. 공개 텍스트 API의 기준은 UTF-8

- 읽기: `to_utf8_string()`, `to_utf8_strings()`
- 쓰기: `from_utf8_view()`, `from_utf8_views()`

`to_string_*` / `from_string_*`는 이미 인코딩된 DICOM raw byte를 다루는 저수준 helper로 유지한다.
`to_utf8_*`는 raw value 전체를 먼저 decode한 뒤 decoded UTF-8 문자열에서 value split을 수행한다.
따라서 raw `0x5C`가 multibyte payload 내부에 나타날 수 있는 GBK / GB18030 / ISO 2022 JIS 계열에서도
raw pre-split로 인한 손상을 만들지 않는다.

### 2. `from_utf8_*`는 즉시 현재 문자셋으로 인코딩한다

이전 설계안의 `StorageKind::utf8_text` 경로는 제거되었다.

현재 동작:

- `DataElement`가 parent dataset에 붙어 있으면, root dataset의 `(0008,0005)`를 기준으로 즉시 인코딩한다.
- parent가 없으면 default repertoire 기준으로 처리한다.
- 현재 문자셋으로 표현할 수 없는 문자는 `from_utf8_*` 호출 시점에 실패한다.

이 모델의 의도는 다음과 같다.

- 인코딩 오류를 write 시점이 아니라 mutation 시점에 노출한다.
- `write_*()`는 raw byte 직렬화만 담당하게 한다.
- dataset 내부 상태를 이미 현재 선언에 맞게 인코딩된 값으로 유지한다.

### 3. 문자셋 선언 변경과 실제 변환을 분리한다

공개 API는 아래 두 함수로 분리한다.

- `set_declared_specific_charset(...)`
  - `(0008,0005)`만 수정한다.
  - 기존 text value raw byte는 변경하지 않는다.
  - 잘못된 메타데이터를 교정할 때 사용한다.
- `set_specific_charset(...)`
  - text VR value를 현재 source charset에서 target charset으로 실제 변환한다.
  - `(0008,0005)`도 target에 맞게 갱신한다.

즉 선언만 바꾸는 것과 데이터를 실제로 변환하는 것을 명확히 구분한다.

### 4. write 경로에서는 charset 변환을 하지 않는다

이전 설계안의 write-time prepare/materialize 경로는 제거되었다.

제거된 항목:

- `StorageKind::utf8_text`
- `PreparedCharsetWrite`
- `prepare_dataset_text_for_write()`
- `prepared_value_bytes()`
- `WriteOptions.target_specific_character_set`
- `WriteOptions.target_specific_character_sets`

현재 `write_bytes()` / `write_file()` / `write_to_stream()`는 dataset에 저장된 raw byte를 그대로 기록한다.

### 5. 문자셋 정의의 기준은 registry 하나로 통일한다

문자셋 관련 기준 정보는 `specific_character_set_registry`에서만 가져온다.

포함되는 정보:

- enum 값
- defined term
- `uses_iso_2022`
- `base_character_set`
- `code_element`
- `escape_sequence_bytes`

별도의 하드코딩 문자셋 목록이나 중복 분류표는 유지하지 않는다.

## 현재 코드 구조

### DataElement

관련 파일:

- `include/dicom.h`
- `src/dataelement.cpp`

현재 역할:

- `from_utf8_view()` / `from_utf8_views()`에서 현재 문자셋 기준 즉시 인코딩
- `to_utf8_string()` / `to_utf8_strings()`에서 raw value 전체를 UTF-8로 decode한 뒤 split
- `to_string_view()`는 raw helper 유지
- `to_string_views()`는 raw `0x5C` split이 unsafe한 declared multibyte charset에서는 `nullopt`

### Charset mutation helper

관련 파일:

- `src/charset/text_validation.hpp`
- `src/charset/charset_decode.hpp`
- `src/charset/charset_mutation.hpp`
- `src/charset/charset_mutation.cpp`

현재 역할:

- raw byte -> UTF-8 decode
- UTF-8 -> 현재/목표 charset encode
- `(0008,0005)` 선언 검증
- dataset 전체 transcode 준비 및 commit

### Charset codec 세부 구현

관련 파일:

- `src/charset/charset_codec.cpp`
- `src/charset/charset_sbcs.cpp`
- `src/charset/charset_gb.cpp`
- `src/charset/charset_iso2022.cpp`
- `src/charset/charset_detail.hpp`

현재 역할:

- UTF-8 codepoint helper
- SBCS 계열 변환
- GBK / GB18030
- ISO 2022 state machine
- JIS / KS X 1001 multibyte mapping

### Writer

관련 파일:

- `src/writing/direct_write.cpp`

현재 역할:

- raw `value_span()` 기반 직렬화
- file meta / transfer syntax 처리
- charset 변환은 수행하지 않음

### Dump

관련 파일:

- `src/dumpdataset.cpp`

현재 정책:

- dump는 `to_utf8_strings()`를 우선 사용한다.
- 즉 `(0008,0005)`를 해석한 결과를 기준으로 표시한다.

## 지원 문자셋 범위

현재 지원:

- `ISO_IR 192`
- `ISO_IR 100`
- `ISO_IR 101`
- `ISO_IR 109`
- `ISO_IR 110`
- `ISO_IR 144`
- `ISO_IR 127`
- `ISO_IR 126`
- `ISO_IR 138`
- `ISO_IR 148`
- `ISO_IR 203`
- `ISO_IR 13`
- `ISO_IR 166`
- `GBK`
- `GB18030`
- `ISO 2022 IR 13`
- `ISO 2022 IR 58`
- `ISO 2022 IR 87`
- `ISO 2022 IR 100`
- `ISO 2022 IR 101`
- `ISO 2022 IR 109`
- `ISO 2022 IR 110`
- `ISO 2022 IR 144`
- `ISO 2022 IR 166`
- `ISO 2022 IR 203`
- `ISO 2022 IR 149`
- `ISO 2022 IR 159`
- multi-term `Specific Character Set`

구현된 규칙:

- `PN`의 `^`, `=` reset 처리
- multi-value `\` 경계 처리
- `ISO 2022` initial `G0` / `G1` designation omission 정책
- raw text -> UTF-8 decode
- UTF-8 -> current/target charset encode
- same-charset raw reuse 최적화

정책:

- non-ISO 2022 multi-term 조합은 reject
- declared되지 않은 ISO 2022 escape라도 지원되는 escape면 decode 허용
- 지원되지 않는 ISO 2022 escape는 reject

## `from_utf8_*` 에러 처리 방향

`from_utf8_view()` / `from_utf8_views()`는 현재 다음 에러 정책을 지원한다.

- `strict`
  - 표현 불가 문자가 하나라도 있으면 실패
  - 기존 값은 변경하지 않음
- `replace_qmark`
  - 표현 불가 문자를 ASCII `?`로 치환해서 저장
- `replace_unicode_escape`
  - 표현 불가 문자를 `(U+XXXX)` 또는 `(U+XXXXXX)` 형식의 ASCII 치환 문자열로 저장

`ignore`는 현재 계획에서 제외한다.

이유:

- 표현 불가 문자만 제거하면 값의 의미와 구분자 의미가 쉽게 훼손된다.
- 값을 바꾸지 않고 그냥 성공시키는 정책은 API 의미가 불명확하다.
- 값을 바꾸지 않고 실패시키는 정책은 사실상 `strict`와 다르지 않다.

## `to_utf8_*` 에러 처리 방향

`to_utf8_string()` / `to_utf8_strings()`는 현재 다음 에러 정책을 지원한다.

- `strict`
  - decode 실패 시 `nullopt`
- `replace_fffd`
  - 잘못된 byte, 불완전 multibyte, 지원되지 않는 escape를 `U+FFFD`로 치환
- `replace_hex_escape`
  - 잘못된 원본 byte 또는 escape sequence를 ASCII 문자열로 노출
  - 예: `(0x81)`, `(0x1B)(0x24)(0x42)`

`ignore`는 현재 계획에서 제외한다.

이유:

- decode 중 문제 byte를 버리면 문자열이 조용히 짧아진다.
- PN, multi-value, 일반 문자열 모두에서 의미 손실을 숨기기 쉽다.
- 읽기 API는 실패를 숨기기보다 치환 결과가 보이게 하는 쪽이 낫다.

### `to_utf8_*` 에러별 예시

#### 1. SBCS에서 unmapped byte

가정:

- `(0008,0005) = ISO_IR 100`
- raw bytes = `41 81 42`

결과:

- `strict`
  - 실패, `nullopt`
- `replace_fffd`
  - `A�B`
- `replace_hex_escape`
  - `A(0x81)B`

#### 2. GBK에서 불완전 multibyte

가정:

- `(0008,0005) = GBK`
- raw bytes = `41 81`

결과:

- `strict`
  - 실패
- `replace_fffd`
  - `A�`
- `replace_hex_escape`
  - `A(0x81)`

또 다른 예:

- `(0008,0005) = GBK`
- raw bytes = `41 81 20`

결과:

- `strict`
  - 실패
- `replace_fffd`
  - `A�`
- `replace_hex_escape`
  - `A(0x81)(0x20)`

#### 3. ISO 2022에서 지원되지 않는 escape

가정:

- `(0008,0005) = ISO 2022 IR 100`
- raw bytes = `41 1B 24 28 44 42`

결과:

- `strict`
  - 실패
- `replace_fffd`
  - `A�B`
- `replace_hex_escape`
  - `A(0x1B)(0x24)(0x28)(0x44)B`

#### 4. ISO 2022에서 escape 뒤 payload가 잘림

가정:

- `(0008,0005) = ISO 2022 IR 87`
- raw bytes = `1B 24 42 30`

결과:

- `strict`
  - 실패
- `replace_fffd`
  - `�`
- `replace_hex_escape`
  - `(0x1B)(0x24)(0x42)(0x30)`

#### 5. multi-value에서 일부 값만 손상됨

가정:

- `(0008,0005) = ISO_IR 100`
- raw bytes = `44 4F 45 5E 4A 4F 48 4E 5C 81 42`

결과:

- `strict`
  - 전체 실패
- `replace_fffd`
  - `["DOE^JOHN", "�B"]`
- `replace_hex_escape`
  - `["DOE^JOHN", "(0x81)B"]`

## `set_specific_charset()` 에러 처리 방향

`set_specific_charset()`은 현재 다음 에러 정책을 지원한다.

- `strict`
  - source decode 또는 target encode 오류가 하나라도 있으면 전체 실패
  - dataset은 변경하지 않음
  - 구현은 prepare-then-commit 경로를 사용한다.
  - 주 용도는 개발 및 디버그 검증이다.
- `replace_qmark`
  - 표현 불가 문자나 decode 오류를 ASCII `?` 치환으로 처리한다.
  - charset 변환 오류 때문에 중간에 실패하지 않는 생산 경로로 사용한다.
  - 현재 구현은 순회하면서 즉시 값 교체하는 fast path를 사용한다.
- `replace_unicode_escape`
  - 표현 불가 문자는 `(U+XXXX)` 또는 `(U+XXXXXX)`로 치환한다.
  - decode 오류 byte는 `(0xNN)` 또는 `(0xNNNN)`로 치환한다.
  - charset 변환 오류 때문에 중간에 실패하지 않는 생산 경로로 사용한다.
  - 현재 구현은 순회하면서 즉시 값 교체하는 fast path를 사용한다.

`ignore`는 현재 계획에서 제외한다.

이유:

- 일부 element만 그대로 두고 나머지 element만 target charset으로 바꾸면 선언과 실제 값이 불일치하는 dataset이 된다.
- 원본을 그대로 보존하고 싶다면 `set_specific_charset()`이 아니라 transcode를 시도하지 않거나, 호출자 레벨에서 실패 시 원본을 유지하면 된다.

주의:

- `replace_*` 모드가 charset 변환 오류 때문에 예외를 던지지 않는다는 뜻이지, 메모리 부족이나 내부 불변식 위반 같은 시스템 오류까지 무조건 억제한다는 뜻은 아니다.
- 구현은 charset 변환 오류를 치환으로 흡수하고 계속 진행하는 것을 목표로 한다.

## 권장 사용 방향

- 새로 생성하거나 정규화하는 데이터는 가능하면 `ISO_IR 192`를 우선 권장한다.
- UTF-8은 다른 단일 바이트 문자셋보다 표현 범위가 넓어서 encode 실패 가능성이 낮다.
- 운영 환경에서는 `set_specific_charset()`의 `replace_*` 모드를 기본 경로로 두고, `strict`는 개발 및 진단 경로로 쓰는 것이 적절하다.

## 문자 테이블 생성 파이프라인

관련 위치:

- `misc/charset/README.md`
- `misc/charset/generate_selected_sbcs_tables.py`
- `misc/charset/generate_gb18030_tables.py`
- `misc/charset/generate_ksx1001_tables.py`
- `misc/charset/generate_jis_tables.py`

생성 대상:

- `src/charset/generated/sbcs_to_unicode_selected.hpp`
- `src/charset/generated/gb18030_tables.hpp`
- `src/charset/generated/ksx1001_tables.hpp`
- `src/charset/generated/jisx0208_tables.hpp`
- `src/charset/generated/jisx0212_tables.hpp`

회귀 테스트:

- `charset_generator_regression`
- `charset_generated_tables_smoke`

## Python binding 기준 동작

관련 파일:

- `bindings/python/dicom_module.cpp`
- `bindings/python/dicomsdl/_dicomsdl.pyi`
- `tests/python/test_charset.py`

현재 정책:

- `DataElement.from_utf8_view()` / `from_utf8_views()`는 C++과 동일하게 즉시 인코딩한다.
- `DicomFile.write_bytes()` / `write_file()`는 더 이상 target charset override를 받지 않는다.
- 문자셋 선언 변경은 `DicomFile.set_declared_specific_charset()` 또는 `DicomFile.set_specific_charset()`로 수행한다.

## 남은 정리 항목

- [x] `from_utf8_view()` / `from_utf8_views()`에 `strict`, `replace_qmark`, `replace_unicode_escape` 정책 추가
- [x] `to_utf8_string()` / `to_utf8_strings()`에 `strict`, `replace_fffd`, `replace_hex_escape` 정책 추가
- [x] `set_specific_charset()`에 `strict`, `replace_qmark`, `replace_unicode_escape` 정책 추가
- [ ] 관련 예제 문서에서 old write-time target charset 설명이 남아 있지 않은지 주기적으로 확인
- [ ] 새 문서와 예제에서 `ISO_IR 192` 우선 권장 문구를 반영

## 검증 상태

현재 구조는 아래 테스트로 검증한다.

- `basic_smoke`
- `pixel_io_smoke`
- `charset_smoke`
- `charset_generated_tables_smoke`
- `charset_generator_regression`
- `dictionary_generator_regression`
- `tests/python/test_basic_smoke.py`
- `tests/python/test_charset.py`
