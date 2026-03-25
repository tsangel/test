# 오류 처리

이 페이지는 여러 가이드에 흩어져 있는 오류 처리 패턴을 한곳에 모아 둔 문서입니다. 다음 두 질문에 빠르게 답을 찾고 싶을 때 보세요.

- 어떤 API가 예외를 발생시키나요?
- 실패하면 다음에 어떻게 해야 합니까?

## 실패 유형

dicomsdl 공개 API는 세 가지 방식으로 실패를 알립니다.

- 예외 발생
  - 상위 수준의 C++ 읽기, 쓰기, 디코드, 인코드, 데이터세트 전체 문자 집합 변경 API는 보통 `dicom::diag::DicomException`으로 실패를 알립니다.
  - Python에서는 같은 런타임 실패가 주로 `RuntimeError`로 보이고, 바인딩 레벨의 인수 검증 오류는 `TypeError`, `ValueError`, `IndexError`로 보고됩니다.
- 반환값 기반 실패
  - 일부 요소 수준 문자 집합 API는 예외 대신 `false`, `None`, 빈 `optional` 같은 반환값으로 실패를 알립니다.
- 오류 상태를 남기는 부분 성공
  - `read_file(..., keep_on_error=True)`와 `read_bytes(..., keep_on_error=True)`는 `DicomFile`을 반환할 수 있지만, 해당 객체에는 `has_error`와 `error_message`가 설정됩니다.

## 예외 처리 패턴

**C++**

```cpp
try {
    // 상위 수준 dicomsdl 작업
} catch (const dicom::diag::DicomException& ex) {
    // 사용자에게 직접 노출되는 DICOM, 코덱, 파일 I/O 오류
} catch (const std::exception& ex) {
    // 더 낮은 수준의 인수/사용 오류 또는 플랫폼 실패
}
```

**파이썬**

```python
import dicomsdl as dicom

try:
    # 상위 수준 dicomsdl 작업
    ...
except TypeError as exc:
    # 잘못된 인수 타입 또는 버퍼/경로 사용 오류
    ...
except ValueError as exc:
    # 잘못된 텍스트 옵션, 잘못된 버퍼/레이아웃 요청, 잘못된 호출
    ...
except IndexError as exc:
    # 프레임 또는 구성 요소 인덱스가 범위를 벗어났습니다.
    ...
except RuntimeError as exc:
    # 기본 C++ 파싱, 디코드, 인코드, 트랜스코드, 쓰기 실패
    ...
```

## 파일 I/O

구문 분석 문제로 인해 파일을 즉시 거부해야 하는 경우 `keep_on_error=False`를 사용하세요. 나중에 파일 형식이 잘못된 것으로 판명되더라도 초기 메타데이터가 여전히 유용한 경우 `keep_on_error=True`를 사용하세요.

### `keep_on_error=False`: 빠른 실패

- 가져오기 파이프라인, 검증 작업 또는 잘못된 파일이 즉시 중지되어야 하는 모든 워크플로에 이 기능을 사용하세요.
- 모든 예외를 "이 파일은 계속 처리하기에 안전하지 않습니다."로 처리하십시오.
- 경로와 예외 텍스트를 기록한 다음 파일을 격리, 건너뛰기 또는 보고합니다.

### `keep_on_error=True`: 이미 구문 분석된 내용을 유지합니다.

- 초기 태그의 이점을 누릴 수 있는 크롤러, 메타데이터 인덱싱, 분류 도구 또는 복구 도구에 이 기능을 사용하세요.
- 모든 허용 읽기 후에는 결과를 신뢰하기 전에 `has_error` 및 `error_message`를 확인하세요.
- `has_error`가 true인 경우 객체를 부분적으로 읽거나 오염된 것으로 처리합니다.
  - 의도적으로 복구하려는 메타데이터만 사용하십시오.
  - 픽셀 디코딩, 픽셀 인코딩 또는 다시 쓰기 흐름을 맹목적으로 계속하지 마십시오.
  - 완전히 신뢰할 수 있는 개체가 필요한 경우 수리 후 엄격하게 다시 로드
- `keep_on_error`는 일반적인 "모든 오류 무시" 스위치가 아닙니다. 경로/열기 실패, 유효하지 않은 Python 버퍼 계약 및 유사한 경계 오류는 여전히 즉시 발생합니다.

### 예

**C++**

```cpp
#include <dicom.h>
#include <diagnostics.h>
#include <iostream>

try {
    dicom::ReadOptions opts;
    opts.keep_on_error = true;

    auto file = dicom::read_file("in.dcm", opts);
    if (file->has_error()) {
        std::cerr << "partial read: " << file->error_message() << '\n';
        // 명시적으로 복구하려는 메타데이터만 유지하십시오.
        // 깨끗한 파일인 것처럼 계속해서 디코드/트랜스코딩을 진행하지 마세요.
    }
} catch (const dicom::diag::DicomException& ex) {
    // 파일 열기 실패 또는 keep_on_error가 부분 성공으로 바꾸지 않는 다른 경계 실패
    // 부분 반환 상태로 변환되지 않습니다.
    std::cerr << ex.what() << '\n';
}
```

**파이썬**

```python
import dicomsdl as dicom

df = dicom.read_file("in.dcm", keep_on_error=True)
if df.has_error:
    print("partial read:", df.error_message)
    # 검사하려는 이미 구문 분석된 메타데이터만 사용하세요.
    # 디코드/트랜스코딩/쓰기 작업 흐름 전에 엄격하게 다시 로드하세요.
```

### 예외가 발생할 수 있는 파일 I/O API

| API 계열 | C++ 실패 형태 | 파이썬 레이즈 | 일반적인 이유 |
| --- | --- | --- | --- |
| `read_file(...)` | 엄격한 읽기가 실패하는 경우 `dicom::diag::DicomException`; `keep_on_error=true`를 사용하면 구문 분석 실패가 대신 반환된 `DicomFile`에서 캡처됩니다. | `TypeError`, `RuntimeError` | 경로를 열 수 없거나, 엄격한 구문 분석이 실패하거나, Python 경로 인수가 `str` / `bytes` / `os.PathLike`가 아닙니다. |
| `read_bytes(...)` | 엄격한 읽기가 실패하는 경우 `dicom::diag::DicomException`; `keep_on_error=true`를 사용하면 구문 분석 실패가 대신 반환된 `DicomFile`에서 캡처됩니다. | `TypeError`, `ValueError`, `RuntimeError` | 버퍼가 1차원 연속 바이트형 데이터가 아니며, `copy=False`가 바이트가 아닌 요소와 함께 사용되거나 구문 분석이 실패합니다. |
| `write_file(...)` | `dicom::diag::DicomException` | `TypeError`, `RuntimeError` | 출력 경로가 잘못되었거나, 파일 열기/플러시가 실패했거나, 파일 메타 재구축이 실패했거나, 데이터세트를 현재 상태로 직렬화할 수 없습니다. |
| `write_bytes(...)` | `dicom::diag::DicomException` | `RuntimeError` | 파일 메타 재구축이 실패하거나 현재 데이터세트를 깔끔하게 직렬화할 수 없습니다. |
| `write_with_transfer_syntax(...)` | `dicom::diag::DicomException` | `TypeError`, `ValueError`, `RuntimeError` | 출력 경로가 잘못되었거나, 전송 구문 선택이 잘못되었거나, 인코더 컨텍스트/옵션이 요청과 일치하지 않거나, 트랜스코드가 실패하거나, 출력 쓰기가 실패했습니다. |

## 픽셀 디코드

가장 안전한 디코드 패턴은 다음과 같습니다.

1. `DecodePlan`를 앞쪽에 생성하세요
2. 그 계획에서 목적지를 할당하다
3. 디코드 호출 전반에 걸쳐 동일한 검증된 계획 및 대상 계약을 재사용합니다.

디코드가 실패하면 잘못된 호출자 계약, 오래된 레이아웃 가정 또는 실제 백엔드/런타임 디코드 실패라는 세 가지 버킷 중 하나를 먼저 가정합니다.

### 디코딩에 실패하면 어떻게 해야 할까요?

- 디코딩이 시작되기 전에 검증이 실패하는 경우:
  - 프레임 인덱스, 대상 크기, 연속성 및 `DecodeOptions`를 확인하세요.
- 이전에 좋은 계획이 실패하기 시작하는 경우:
  - 픽셀에 영향을 미치는 메타데이터 변경 후 계획과 대상을 다시 만듭니다.
- 런타임 디코딩이 실패하는 경우:
  - 메시지를 기록하고 이를 단순한 모양 문제가 아닌 파일/코덱 문제로 처리합니다.
- 파이썬에서는:
  - `TypeError`, `ValueError`, `IndexError`는 보통 인수나 요청한 레이아웃이 잘못되었음을 의미합니다.
  - `RuntimeError`는 일반적으로 기본 디코드 경로 자체가 실패했음을 의미합니다.

### 예외가 발생할 수 있는 픽셀 디코드 API

| API 계열 | C++ 실패 형태 | 파이썬 레이즈 | 일반적인 이유 |
| --- | --- | --- | --- |
| `create_decode_plan(...)` | `dicom::diag::DicomException` | `RuntimeError` | 픽셀 메타데이터가 누락되었거나 일관성이 없거나, 명시적인 보폭이 유효하지 않거나, 요청된 디코딩된 레이아웃이 오버플로되었습니다. |
| `decode_into(...)` | `dicom::diag::DicomException` | `TypeError`, `ValueError`, `IndexError`, `RuntimeError` | 프레임 인덱스가 잘못되었거나, 대상이 잘못된 크기 또는 레이아웃이거나, 계획이 더 이상 파일 상태와 일치하지 않거나, 디코더/백엔드가 실패했습니다. |
| `pixel_buffer(...)` | `dicom::diag::DicomException` | 직접 노출되지 않은 | 소유 버퍼 편의 경로에서 `decode_into(...)`와 동일한 기본 디코드 실패 |
| `decode_all_frames_into(...)` | `dicom::diag::DicomException` | `decode_into(..., frame=-1)` 및 `to_array(frame=-1)`가 적용됩니다. | 대상이 너무 작거나, 프레임 메타데이터가 유효하지 않거나, 일괄 디코드/백엔드 실행이 실패합니다. |
| `to_array(...)` | 해당 없음 | `ValueError`, `IndexError`, `RuntimeError` | 잘못된 프레임 요청, 잘못된 디코드 옵션 요청 또는 기본 디코드 실패 |
| `to_array_view(...)` | 해당 없음 | `ValueError`, `IndexError` | 잘못된 프레임 요청, 압축된 소스 데이터 또는 호환 가능한 직접 원시 픽셀 보기 없음 |

## 픽셀 인코딩

가장 안전한 인코딩 패턴은 다음과 같습니다.

1. 긴 루프 전에 대상 전송 구문 및 옵션을 검증하십시오.
2. 동일한 전송 구문과 옵션 세트가 반복되는 경우 `EncoderContext`를 선호합니다.
3. 목표가 단지 다르게 인코딩된 출력 파일인 경우 `write_with_transfer_syntax(...)`를 선호합니다.

### 인코딩 실패 시 대처 방법

- `EncoderContext`를 빌드하는 동안 오류가 발생하는 경우:
  - 실제 인코딩 루프를 시작하기 전에 전송 구문이나 옵션 세트를 수정하세요.
- `set_pixel_data(...)` 중에 오류가 발생하는 경우:
  - 먼저 소스 버퍼 모양, dtype, 연속성 및 픽셀 메타데이터 가정을 확인합니다.
- `set_transfer_syntax(...)` 중에 오류가 발생하는 경우:
  - 현재 객체 상태에 대한 메모리 내 트랜스코드 실패로 처리합니다.
- 목표만 출력되는 경우:
  - 실패한 트랜스코드가 일반적인 인메모리 워크플로가 되지 않도록 `write_with_transfer_syntax(...)`를 선호합니다.

### 예외가 발생할 수 있는 픽셀 인코딩 API

| API 계열 | C++ 실패 형태 | 파이썬 레이즈 | 일반적인 이유 |
| --- | --- | --- | --- |
| `create_encoder_context(...)` / `EncoderContext::configure(...)` | `dicom::diag::DicomException` | `TypeError`, `ValueError`, `RuntimeError` | 전송 구문이 잘못되었거나, 옵션 키/값이 잘못되었거나, 런타임 인코더 구성이 실패했습니다. |
| `set_pixel_data(...)` | `dicom::diag::DicomException` | `TypeError`, `ValueError`, `RuntimeError` | 소스 버퍼 유형/모양/레이아웃이 유효하지 않습니다. 소스 바이트가 선언된 레이아웃과 일치하지 않습니다. 인코더 선택이 실패하거나 인코딩/백엔드 업데이트가 실패합니다. |
| `set_transfer_syntax(...)` | `dicom::diag::DicomException` | `TypeError`, `ValueError`, `RuntimeError` | 전송 구문 선택이 잘못되었거나, 옵션/컨텍스트가 요청과 일치하지 않거나, 트랜스코드/백엔드 경로가 실패했습니다. |
| `write_with_transfer_syntax(...)` | `dicom::diag::DicomException` | `TypeError`, `ValueError`, `RuntimeError` | 잘못된 경로 또는 전송 구문 텍스트, 잘못된 옵션/컨텍스트, 지원되지 않는 트랜스코드 경로, 백엔드 인코딩 실패 또는 출력 쓰기 실패 |

## 문자 세트 및 사람 이름

Charset 처리는 의도적으로 두 가지 스타일을 혼합합니다.

- 요소 수준 읽기/쓰기 도우미는 대부분 `None`, 빈 `optional` 또는 `false`로 인한 일반 오류를 보고합니다.
- 데이터세트 전체 문자 집합 변환은 유효성 검사/트랜스코드 작업이므로, 실패 시 예외를 발생시킵니다.

이러한 차이는 실패가 단지 "이 텍스트 할당 실패"인지 아니면 "이 전체 데이터세트 트랜스코드가 중지되어야 하는지"인지 결정할 때 중요합니다.

### charset 작업이 실패하면 어떻게 해야 할까요?

- `to_utf8_string()` / `to_person_name()`의 경우:
  - 빈 `optional` 또는 `None`를 "디코드/파싱이 사용 가능한 값을 생성하지 않았습니다"로 처리합니다.
  - 엄격한 실패 대신 최선을 다해 텍스트를 원하는 경우 교체 정책을 선택하세요.
- `from_utf8_view()` / `from_person_name()`의 경우:
  - `false`를 "현재 문자 세트/정책에서는 이 쓰기가 성공하지 못했습니다"로 처리합니다.
  - 손실 교체가 허용되고 해당 일이 발생했는지 알고 싶을 때 Python에서 `return_replaced=True`를 사용하거나 C++에서 `bool* out_replaced`를 사용하십시오.
- Python 요소 수준 도우미의 경우:
  - 잘못된 `errors=` 텍스트는 일반 반환 값 경로에 도달하기 전에 여전히 `ValueError`를 발생시킨다는 점을 기억하세요.
- `set_specific_charset()`의 경우:
  - 유효성 검사 또는 빠른 실패 정리를 위해 `strict`를 사용하십시오.
  - 표시된 `(U+XXXX)` 마커를 남겨두고 트랜스코드를 완료하려면 `replace_unicode_escape`를 사용하세요.
  - 현재 데이터세트에 이미 잘못 선언된 원시 바이트가 포함되어 있을 수 있는 경우 일반 트랜스코드를 선언 복구로 처리하는 대신 문제 해결 흐름을 사용하세요.

### 예외가 발생할 수 있는 문자 집합 API

| API 계열 | C++ 실패 형태 | 파이썬 레이즈 | 일반적인 이유 |
| --- | --- | --- | --- |
| `set_specific_charset(...)` | `dicom::diag::DicomException` | `TypeError`, `ValueError`, `RuntimeError` | 문자 집합 선언 문자열이 잘못되었거나, 정책 문자열이 잘못되었거나, 선택한 정책으로 소스 텍스트를 트랜스코드할 수 없거나, 데이터세트 전체 문자 집합 변환이 실패합니다. |
| `set_declared_specific_charset(...)` | `dicom::diag::DicomException` | `TypeError`, `ValueError`, `RuntimeError` | 선언 인수가 유효하지 않거나 `(0008,0005)`를 일관되게 업데이트할 수 없습니다. 주로 수리/문제 해결 흐름에 사용 |

### 일반적인 콘텐츠 오류에 대해 throw하지 않는 Charset API

| API 계열 | C++ 실패 형태 | Python 실패 형식 | 일반적인 의미 |
| --- | --- | --- | --- |
| `to_utf8_string()` / `to_utf8_strings()` | 빈 `std::optional` | `None` 또는 `(None, replaced)` | VR이 잘못되었거나 문자 세트 디코딩에 실패했거나 사용 가능한 디코딩 텍스트가 생성되지 않았습니다. |
| `to_person_name()` / `to_person_names()` | 빈 `std::optional` | `None` 또는 `(None, replaced)` | 잘못된 VR, 문자 세트 디코드 실패 또는 디코드 후 PN 구문 분석 실패 |
| `from_utf8_view()` / `from_utf8_views()` | `false` | `False` 또는 `(False, replaced)` | 현재 문자 세트 및 오류 정책에 따라 요소 쓰기가 성공하지 못했습니다. |
| `from_person_name()` / `from_person_names()` | `false` | `False` 또는 `(False, replaced)` | 현재 문자 집합 및 오류 정책에 따라 PN 쓰기가 성공하지 못했습니다. |

## 어떤 전략부터 시작해야 할까요?

- 잘못된 파일은 작업 흐름을 즉시 중지해야 합니다.
  - 엄격한 `read_file(...)` / `read_bytes(...)`를 사용하세요.
- 잘못된 형식의 파일에서 메타데이터를 복구하고 싶습니다.
  - `keep_on_error=True`를 사용한 다음 항상 `has_error` 및 `error_message`를 검사하세요.
- 호출자 관리 디코드 버퍼 또는 명시적 출력 스트라이드를 원합니다.
  - `create_decode_plan(...)`와 `decode_into(...)`를 사용하세요.
- 가장 간단한 디코드 경로를 먼저 원합니다
  - Python에서는 `to_array()`를 사용하고 C++에서는 `pixel_buffer()`를 사용하세요.
- 동일한 인코딩 구성으로 많은 출력을 작성 중입니다.
  - 하나의 `EncoderContext`를 구축하세요
- 다른 출력 전송 구문을 원합니다.
  - `write_with_transfer_syntax(...)` 선호
- 데이터세트 전체에서 텍스트 값을 변경하거나 트랜스코딩하고 있습니다.
  - `set_specific_charset(...)` 사용
- 하나의 텍스트 요소를 읽거나 쓰고 있는데 일반적인 예/아니요 실패를 원합니다.
  - `to_utf8_string()` / `from_utf8_view()` 및 해당 PN 변형을 사용하세요.

## 관련 문서

- [파일 I/O](file_io.md)
- [픽셀 디코드](pixel_decode.md)
- [픽셀 인코딩](pixel_encode.md)
- [문자셋 및 사람 이름](charset_and_person_name.md)
- [문제 해결](troubleshooting.md)
- [오류 모델](../reference/error_model.md)
