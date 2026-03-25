# UID 생성 중

이 문서에서는 DicomSDL의 현재 UID 생성 및 추가 흐름을 설명합니다.

## 1. 범위

- C++:
  - `dicom::uid::try_generate_uid()`
  - `dicom::uid::generate_uid()`
  - `dicom::uid::Generated::try_append(component)`
  - `dicom::uid::Generated::append(component)`
- 파이썬:
  - `dicom.try_generate_uid()`
  - `dicom.generate_uid()`
  - `dicom.try_append_uid(base_uid, component)`
  - `dicom.append_uid(base_uid, component)`

## 2. C++ API

### 2.1 기본 UID 생성

```cpp
auto uid_opt = dicom::uid::try_generate_uid(); // std::optional<Generated>
auto uid = dicom::uid::generate_uid();         // Generated (throws on failure)
```

- 접두사: `dicom::uid::uid_prefix()`
- `generate_uid()`는 UID를 `<uid_prefix>.<random_numeric_suffix>`로 빌드합니다.
접미사는 프로세스 수준의 무작위 논스 + 단조로운 시퀀스에서 생성됩니다.
- 출력: 엄격하게 유효한 UID 텍스트(최대 64자)

### 2.2 하나의 구성 요소 추가

```cpp
auto study = dicom::uid::generate_uid();
auto series = study.append(23);
auto inst = series.append(34);
```

- `try_append(component)`는 `std::optional<Generated>`를 반환합니다.
- `append(component)`는 실패 시 `std::runtime_error`를 발생시킵니다.

### 2.3 `Generated`에 대한 기존 UID 텍스트

```cpp
auto base = dicom::uid::make_generated("1.2.840.10008");
if (base) {
    auto extended = base->append(7);
}
```

## 3. 파이썬 API

```python
import dicomsdl as dicom

study = dicom.generate_uid()
series = dicom.append_uid(study, 23)
inst = dicom.append_uid(series, 34)

safe = dicom.try_append_uid("1.2.840.10008", 7)  # Optional[str]
```

- 잘못된 입력/실패 시 `append_uid(...)` 발생
- `try_append_uid(...)`는 실패 시 `None`를 반환합니다.

## 4. 동작 추가

`append` / `try_append`의 경우:

1. 직접 경로:
   - 먼저 `<base_uid>.<component>`를 사용해 보세요.
   - 유효하고 <= 64자이면 반환합니다.

2. 대체 경로(직접 추가가 맞지 않는 경우):
   - `base_uid`의 처음 30자를 유지합니다.
   - 마지막 문자가 `.`가 아닌 경우 `.`를 추가합니다.
   - 하나의 U96 10진수 블록을 추가합니다.
   - 결과는 엄격한 UID 유효성 검사기에 의해 재검증됩니다.

### 중요사항

대체 출력은 의도적으로 비결정적입니다.

- 여전히 `component` 및 `base_uid`를 기반으로 하며,
- 프로세스 수준의 임의 논스 + 원자 시퀀스도 혼합합니다.

따라서 동일한 `(base_uid, component)`의 경우 대체 접미사는 호출마다 다를 수 있습니다.

## 5. 실패 모델 요약

- `generate_uid()`:
  - 실패하면 던진다
- `try_generate_uid()`:
  - 실패 시 `None` / `std::nullopt`를 반환합니다.
- `append_uid()` / `Generated::append()`:
  - 실패하면 던진다
- `try_append_uid()` / `Generated::try_append()`:
  - 실패 시 `None` / `std::nullopt`를 반환합니다.

## 6. 실용적인 추천

- 신청 흐름:
  - `generate_uid()`로 기반 구축
  - `append(...)`를 통해 하위 UID 파생
- 입력 기반 UID를 신뢰할 수 없는 경우 다음을 사용하세요.
  - Python의 `try_append_uid(...)`
  - C++의 `try_append(...)`
