# Python 데이터세트 가이드

DicomSDL은 얇은 nanobind 래퍼입니다. 런타임에 네이티브 확장을 로드하므로 문서는 mock import로 빌드됩니다. 예제를 실행하려면 휠을 설치하세요.

이것은 DicomSDL의 Python 측 파일, 데이터 세트 및 요소 액세스에 대한 주요 사용자 대상 가이드입니다.
모듈 수준 진입점, DicomSDL 개체가 DICOM에 매핑되는 방법, 가장 중요한 읽기/쓰기 패턴을 다룹니다.

## 가져오기

```python
import dicomsdl as dicom
```

## 모듈 수준 진입점

- `keyword_to_tag_vr(keyword: str) -> (Tag, VR)`: 키워드를 `(Tag, VR)`로 해결합니다.
- `tag_to_keyword(tag: Tag | str) -> str`: 태그를 키워드로 해석합니다.
- `read_file(path) -> DicomFile`: 디스크에서 DICOM 파일/세션을 로드합니다.
- `read_bytes(data, name="inline") -> DicomFile`: 메모리 내 버퍼에서 로드합니다.
- `generate_uid() -> str`: DICOMSDL 접두사 아래에 새 UID를 생성합니다.
- `append_uid(base_uid: str, component: int) -> str`: 대체 정책이 포함된 하나의 UID 구성 요소를 추가합니다.

## DicomSDL이 DICOM에 매핑되는 방법

DicomSDL은 관련된 Python 개체의 작은 집합을 노출합니다.

- `DicomFile`: 루트 데이터세트를 소유하는 파일/세션 래퍼
- `DataSet`: DICOM `DataElement` 개체의 컨테이너
- `DataElement`: 태그 / VR / 길이 메타데이터와 타입 값 접근을 제공하는 하나의 DICOM 필드
- `Sequence`: `SQ` 값에 대한 중첩 항목 컨테이너
- `PixelSequence`: 캡슐화되거나 압축된 픽셀 데이터를 위한 프레임/조각 컨테이너

개체 모델 및 DICOM 매핑은 [핵심 개체](core_objects.md)를 참조하세요.

바인딩은 의도적으로 분할 모델을 사용합니다.

- 속성 액세스는 값 지향적입니다: `ds.Rows`
- 인덱스 액세스는 `DataElement`: `ds["Rows"]`를 반환합니다.

이를 통해 일반적인 읽기를 짧게 유지하면서 VR/길이/태그 메타데이터를 쉽게 검사할 수 있습니다.

### DicomFile 및 데이터세트

대부분의 데이터 요소 액세스 API는 `DataSet`에서 구현됩니다.
`DicomFile`는 루트 `DataSet`를 소유하고 로드, 저장, 트랜스코드와 같은 파일 지향 작업을 처리합니다.
편의상 `DicomFile`는 루트 데이터 세트 액세스를 전달하므로 `df.Rows`, `df["Rows"]`, `df.get_value(...)` 및 `df.Rows = 512`는 모두 `df.dataset`에 위임됩니다.
`ds = df.dataset`를 바인딩하면 전달하지 않고 동일한 데이터세트 API를 직접 사용하게 됩니다.

다음 패턴은 동일합니다.

```python
df = dicom.read_file("sample.dcm")

rows1 = df.Rows
rows2 = df.dataset.Rows

elem1 = df["Rows"]
elem2 = df.dataset["Rows"]

df.Rows = 512
df.dataset.Rows = 512
```

## 권장 API

| API | 반환값 | 누락 시 동작 | 용도 |
| --- | --- | --- | --- |
| `"Rows" in ds` | `bool` | `False` | 존재 여부 확인 |
| `ds.get_value("Rows", default=None)` | 타입이 지정된 값 또는 전달한 기본값 | 전달한 기본값을 반환 | `None` 또는 다른 기본값으로 누락을 표현하는 일회성 타입 읽기 |
| `ds["Rows"]`, `ds.get_dataelement("Rows")` | `DataElement` | `False`로 평가되는 `NullElement`를 반환, 예외는 없음 | `DataElement` 접근 |
| `ds.ensure_loaded("Rows")` | `None` | 잘못된 키면 예외 발생 | `Rows` 같은 이후 태그까지 부분 읽기를 명시적으로 진행 |
| `ds.ensure_dataelement("Rows", vr=None)` | `DataElement` | 기존 요소를 반환하거나 길이가 0인 요소를 삽입 | 체인에 친화적인 ensure/create API |
| `ds.set_value("Rows", 512)` | `bool` | 쓰기 실패 시 `False`, `None`으로 길이 0 값 설정 가능 | 일회성 할당 |
| `ds.set_value(0x00090030, dicom.VR.US, 16)` | `bool` | 명시적 VR로 생성 또는 재정의 | 비공개 또는 모호한 태그 |
| `ds.Rows` | 타입이 지정된 값 | `AttributeError` | 알려진/자주 쓰는 태그를 위한 개발/대화형 편의 접근 |
| `ds.Rows = 512` | `None` | 할당 실패 시 예외 발생 | 표준 키워드 업데이트를 위한 개발/대화형 편의 할당 |

## Python에서 데이터 요소를 식별하는 방법

| 형식 | 예시 | 먼저 고려할 상황 |
| --- | --- | --- |
| 압축된 정수 | `0x00280010` | 태그가 숫자 테이블이나 외부 메타데이터에서 올 때 |
| 키워드 또는 태그 문자열 | `"Rows"`, `"(0028,0010)"` | 대부분의 일반적인 Python 코드 |
| 점으로 구분된 태그 경로 문자열 | `"RadiopharmaceuticalInformationSequence.0.RadionuclideTotalDose"` | 한 단계에서 중첩 조회나 할당을 하고 싶을 때 |
| `Tag` 객체 | `dicom.Tag("Rows")`, `dicom.Tag(0x0028, 0x0010)` | 명시적으로 재사용 가능한 태그 객체가 필요할 때 |

### `0x00280010`

- 태그가 숫자 상수, 생성된 테이블, 외부 메타데이터에서 바로 올 때 쓰기 좋습니다.
- 장점: 단일 태그에 대해 가장 빠르고 직접적인 경로이며 문자열 파싱이 없고, `ensure_loaded(...)` 같은 단일 태그 API에서도 잘 맞습니다.
- 트레이드오프: 키워드보다 읽기 어렵고 중첩 경로는 표현할 수 없습니다.

### `"Rows"` 또는 `"(0028,0010)"`

- 대부분의 일반적인 Python 코드에서 먼저 쓰기 좋습니다.
- 장점: 짧고 읽기 쉬우며 일반적인 조회/쓰기 API 전반에서 잘 동작합니다.
- 트레이드오프: 키워드/태그 문자열을 런타임에 해석하는 비용이 조금 있고, 중첩 접근에는 점으로 구분된 경로 문자열이 필요합니다.

### 점으로 구분된 태그 경로 문자열

- 한 단계에서 중첩 조회나 할당을 하고 싶을 때 쓰기 좋습니다.
- 예: `"RadiopharmaceuticalInformationSequence.0.RadionuclideTotalDose"`, `"00540016.0.00181074"`.
- 장점: 중첩 데이터세트로 들어가는 one-shot 접근을 읽기 쉽게 표현할 수 있습니다.
- 트레이드오프: 중첩 경로를 지원하는 API에서만 쓸 수 있고, `ensure_loaded(...)`, `remove_dataelement(...)` 같은 일반 태그 전용 API에서는 지원하지 않습니다.

### `dicom.Tag(...)`

- 명시적인 태그 객체가 필요하거나 같은 태그를 여러 번 재사용할 때 쓰기 좋습니다.
- 장점: 타입이 명시적이고 재사용 가능하며 API 경계에서 분명합니다.
- 트레이드오프: 일회성 호출에서는 Python 수준 `Tag` 객체를 추가로 만들어야 하므로, 재사용이 필요 없으면 압축된 정수가 더 직접적입니다.

실용적인 권장사항:

- 대부분의 Python 코드에서 일반적인 키워드/태그 문자열 액세스에는 `"Rows"`를 사용합니다. 여기에는 여전히 런타임 키워드/태그 구문 분석 비용이 적지만 DicomSDL은 최적화된 런타임 키워드 경로와 일반 키워드 문자열에 대한 더 가벼운 직접 경로를 사용하므로 오버헤드가 일반적으로 작습니다.
- 단일 태그가 이미 숫자 상수 또는 외부 메타데이터에서 나온 경우 또는 단일 태그에 대한 가장 빠른 경로를 원하는 경우 압축된 정수를 사용합니다.
- 한 단계에서 중첩된 값이나 할당을 원할 경우 점으로 구분된 태그 경로 문자열을 사용합니다. Python에서는 순회가 하나의 C++ 경로 구문 분석/조회 호출 내에 유지되므로 반복적으로 중첩된 `Sequence` / `DataSet` API 호출보다 더 빠를 수도 있습니다.
- 명시적으로 재사용 가능한 태그 객체를 원할 때 `dicom.Tag(...)`를 사용하세요.
- `ds.Rows`는 개발 또는 대화형 탐색에서 편리하며, `dir()`가 현재 존재하는 공개 표준 키워드를 노출하므로 많은 대화형 셸에서 탭 완성과도 잘 맞습니다. 다만 키워드가 잘못되었거나 요소가 누락되면 `AttributeError`가 발생합니다. 프로덕션 코드에서는 string/int/`Tag` 키가 보통 더 명시적이고 다루기 쉽습니다.

## 값 읽기

### 속성 액세스는 입력된 값을 반환합니다.

```python
rows = ds.Rows
patient_name = ds.PatientName
```

요소가 존재할 것으로 예상하고 메타데이터가 아닌 실제 값을 원할 때 이를 사용하십시오.

### 인덱스 액세스는 DataElement를 반환합니다.

```python
elem = ds["Rows"]
if elem:
    print(elem.tag, elem.vr, elem.length, elem.value)
```

누락된 조회는 예외를 발생시키는 대신 `False`로 평가되는 객체를 반환합니다.

```python
missing = ds["NotARealKeyword"]
assert not missing
assert missing.value is None
```

### 존재 여부 확인

요소가 존재하는지 여부만 알아야 하는 경우 `in`를 사용하세요.

```python
if "Rows" in ds:
    rows = ds["Rows"].value

if dicom.Tag("PatientName") in df:
    print(df["PatientName"].value)
```

허용되는 키 유형은 다음과 같습니다.

- `str` 키워드 또는 태그 경로 문자열
- `Tag`
- `0x00280010`와 같은 `int`로 압축됨

잘못된 키워드/태그 문자열은 `False`를 반환합니다.

### 메소드 형식에서 동일한 조회: get_dataelement(...)

`get_dataelement(...)`는 `ds[...]`와 동일한 조회를 수행합니다. 일부 코드베이스는 더 명확하게 읽을 수 있는 명명된 메서드 형식을 선호합니다.

```python
elem = ds.get_dataelement("PatientName")
if elem:
    print(elem.vr, elem.length, elem.value)
```

`ds[...]`와 동일한 누락 요소 감시 동작을 사용합니다.

### 부분 로드 지속

현재 필요한 태그 이전에 부분 읽기가 중지된 경우 `ensure_loaded(...)`를 사용하세요.

```python
df.ensure_loaded("Rows")
df.dataset.ensure_loaded(dicom.Tag("Columns"))
```

허용되는 키 유형은 다음과 같습니다.

- `Tag`
- `0x00280010`와 같은 `int`로 압축됨
- `"Rows"` 또는 `"(0028,0010)"`와 같은 키워드 또는 태그 문자열

중첩된 점 태그 경로 문자열은 `ensure_loaded(...)`에서 지원되지 않습니다.

### 빠른 경로: get_value()

`None`와 같은 기본값이 누락된 요소를 나타내는 일회성 값 읽기에는 `get_value()`를 사용합니다.

```python
rows = ds.get_value("Rows")
window_center = ds.get_value("WindowCenter", default=None)
```

이는 `DataElement` 개체가 필요하지 않은 경우 가장 짧은 비상승 값 경로입니다.
요소가 누락된 경우 `default`를 다시 가져옵니다.

`get_value()`는 암시적으로 부분 로딩을 계속하지 않습니다. 파일 기반 데이터세트가
이전 태그까지만 로드되었으며 이후 태그를 쿼리하면 현재 사용 가능한 태그가 반환됩니다.
상태. 부분 읽기 후 `Rows`와 같은 이후 태그가 필요한 경우 먼저 `ensure_loaded(...)`를 호출하세요.

전달한 기본값은 누락된 요소에만 사용됩니다. 길이가 0인 값을 가진 데이터 요소는 여전히 타입이 지정된 빈 값을 반환합니다.

```python
assert ds.get_value("PatientName", default="DEFAULT") == "DEFAULT"  # missing
assert ds.get_value("Rows", default="DEFAULT") == []                # present, zero-length US
```

### 연결된 경로: ds["Rows"].value

메타데이터와 값을 함께 봐야 할 때 이 경로를 사용하세요:

```python
rows_elem = ds["Rows"]
if rows_elem:
    print(rows_elem.vr)
    print(rows_elem.value)
```

### `DataElement.value` / `get_value()`의 반환 타입

- `SQ` / `PX` -> `Sequence` / `PixelSequence`
- 숫자형 VR(`IS`, `DS`, `AT`, `FL`, `FD`, `SS`, `US`, `SL`, `UL`, `SV`, `UV`) -> `int`, `float`, `Tag` 또는 `list[...]`
- `PN` -> 파싱 성공 시 `PersonName` 또는 `list[PersonName]`
- 문자 세트 인식 텍스트 VR -> UTF-8 `str` 또는 `list[str]`
- 문자셋 디코드 또는 `PN` 구문 분석 실패 -> 원시 `bytes`
- 바이너리 VR -> 읽기 전용 `memoryview`

길이가 0인 데이터 요소의 경우:

- 숫자와 유사한 VR은 `[]`를 반환합니다.
- 텍스트 VR은 `""`를 반환합니다.
- `PN`는 빈 `PersonName`를 반환합니다.
- 바이너리 VR은 빈 읽기 전용 `memoryview`를 반환합니다.
- `SQ` / `PX`는 빈 컨테이너 객체를 반환합니다.

이는 기본 C++ 벡터 접근자와 일치합니다. 길이가 0인 숫자형 값은 누락된 값이 아닌 구문 분석 가능한 빈 벡터로 처리됩니다.

### 길이가 0인 반환 행렬

가장 중요한 규칙은 길이가 0인 데이터 요소가 여전히 *존재*한다는 것입니다. `default` 인수를 사용하지 않으며 누락된 조회처럼 동작하지 않습니다.

특히 일부 문자열 VR은 일반적으로 `VM > 1`를 가질 수 있지만 `vm()`는 `> 1`가 아니라 `0`이기 때문에 길이가 0인 값은 여전히 ​​빈 스칼라 스타일 값으로 다시 읽습니다.

| VR 가족 | 비어 있지 않은 `VM == 1` | 비어 있지 않은 `VM > 1` | 길이가 0인 값 |
| --- | --- | --- | --- |
| `AE`, `AS`, `CS`, `DA`, `DT`, `TM`, `UI`, `UR` | `str` | `list[str]` | `""` |
| `LO`, `LT`, `SH`, `ST`, `UC`, `UT` | `str` | 다중 값 지원 VR용 `list[str]`; 그렇지 않으면 `str` | `""` |
| `PN` | `PersonName` | 구문 분석 성공 시 `list[PersonName]` | 빈 `PersonName` |
| `IS`, `DS` | `int` / `float` | `list[int]` / `list[float]` | `[]` |
| `AT` | `Tag` | `list[Tag]` | `[]` |
| `FL`, `FD`, `SS`, `US`, `SL`, `UL`, `SV`, `UV` | `int` / `float` | `list[int]` / `list[float]` | `[]` |
| `OB`, `OD`, `OF`, `OL`, `OW`, `OV`, `UN` | `memoryview` | Python 목록 값으로 사용되지 않음 | 빈 `memoryview` |
| `SQ`, `PX` | 시퀀스 객체 | 시퀀스형 컨테이너 | 빈 컨테이너 개체 |

예:

```python
assert ds.get_value("ImageType") == ["ORIGINAL", "PRIMARY"]
assert ds.get_value("ImageType", default="DEFAULT") == ""   # present, zero-length CS

assert ds.get_value("PatientName", default="DEFAULT") == "DEFAULT"  # missing
assert str(ds["PatientName"].value) == ""                           # present, zero-length PN

assert ds.get_value("Rows", default="DEFAULT") == []               # present, zero-length US
assert ds.get_value("WindowCenter", default="DEFAULT") == []       # present, zero-length DS
```

직접 벡터 접근자의 경우 길이가 0인 값은 `None`가 아닌 빈 컨테이너도 반환합니다.

```python
assert ds["Rows"].to_longlong_vector() == []
assert ds["WindowCenter"].to_double_vector() == []
assert ds["FrameIncrementPointer"].to_tag_vector() == []
```

C++ 계층에서는 이제 동일한 계약이 벡터 접근자에 적용됩니다.

```cpp
auto rows = dataset["Rows"_tag].to_longlong_vector();   // engaged optional, empty vector when zero-length
auto wc = dataset["WindowCenter"_tag].to_double_vector();
auto at = dataset["FrameIncrementPointer"_tag].to_tag_vector();
```

### 길이가 0인 것과 누락된 것을 구별하기

DicomSDL에서 `missing`와 `zero-length`는 서로 다른 요소 상태이므로 `elem.value`만 살펴보는 것이 아니라 `DataElement` 수준에서 테스트해야 합니다.

다음 규칙을 사용하세요.

```python
elem = ds["PatientName"]

if not elem:
    # 누락된 조회
elif elem.length == 0:
    # 길이가 0인 현재 요소
else:
    # 비어 있지 않은 값을 가진 현재 요소
```

실질적인 차이점:

- 누락된 요소
- `bool(elem) == False`
- `elem.is_missing() == True`
- `elem.vr == dicom.VR.None`
- `elem.value is None`
- 길이가 0인 현재 요소
- `bool(elem) == True`
- `elem.is_missing() == False`
- `elem.vr != dicom.VR.None`
- `elem.length == 0`

이러한 구별은 "존재하지만 비어 있음"이 "존재하지 않음"과 의미상 다른 DICOM 속성에 중요합니다.

## 데이터요소

`DataElement`는 주요 메타데이터를 포함하는 개체입니다.

### 핵심 속성

- `elem.value`
- `elem.tag`
- `elem.vr`
- `elem.length`
- `elem.offset`
- `elem.vm`

이는 메서드 호출이 아닌 속성입니다.

```python
elem = ds["Rows"]
print(elem.tag)
print(elem.vr)
print(elem.length)
```

### 참/거짓 평가와 누락 요소 객체

```python
elem = ds["PatientName"]
if elem:
    ...

missing = ds["NotARealKeyword"]
assert not missing
assert missing.is_missing()
```

`bool(elem)`는 `elem.is_present()`와 일치합니다.

길이가 0인 현재 요소의 경우 `bool(elem)`는 여전히 `True`입니다. 이를 감지하려면 `elem.length == 0`를 사용하세요.

### 타입별 읽기/쓰기 도우미

- `elem.get_value()`는 `elem.value`를 미러링합니다.
- `elem.set_value(value)`는 `value` 설정자를 미러링하고 `True`/`False`를 반환합니다.
- 실패한 `elem.set_value(value)`는 소유 데이터 세트를 유효하게 유지하지만 대상 요소 상태는 지정되지 않습니다.
- 타입 변환 도우미로는 `to_long()`, `to_double()`, `to_string_view()`, `to_utf8_string()`, `to_utf8_strings()`, `to_person_name()`와 관련 벡터 변형이 있습니다.

### 원시 바이트

`value_span()`는 복사하지 않고 읽기 전용 `memoryview`를 반환합니다.

```python
raw = ds.get_dataelement("PixelData").value_span()
print(raw.nbytes)
```

## 값 쓰기

### ensure-or-create 조회

`ensure_dataelement(...)`는 연결 친화적인 "이 요소가 존재하는지 확인하세요" API입니다.

```python
rows = ds.ensure_dataelement("Rows")
private_value = ds.ensure_dataelement(0x00090030, dicom.VR.US)
```

규칙:

- 요소가 이미 존재하고 `vr`가 생략되거나 `None`인 경우 기존 요소가 변경되지 않고 반환됩니다.
- 요소가 이미 존재하고 `vr`가 명시적이면서 현재 VR과 다르면, 요청한 VR이 보장되도록 기존 요소를 제자리에서 재설정합니다.
- 요소가 누락된 경우 길이가 0인 새 요소가 삽입됩니다.
- `add_dataelement(...)`와 달리 이 API는 명시적인 VR을 시행해야 하는 경우에만 재설정됩니다.
- 부분적으로만 로드된 file-backed 데이터세트에서 아직 파싱되지 않은 태그에 `ensure_dataelement(...)`를 호출하면, 암묵적으로 더 읽지 않고 예외가 발생합니다.

### 반환된 DataElement를 통해 기존 요소 업데이트

```python
ds["Rows"].value = 512
```

요소 개체가 이미 있으면 이는 자연스러운 경로입니다.

### set_value()를 사용한 일회성 할당

```python
assert ds.set_value("Rows", 512)
assert ds.set_value("StudyDescription", "Example")
assert ds.set_value("Rows", None)   # present, zero-length US
```

한 번의 호출로 키를 생성/업데이트하려는 경우 가장 좋은 경로입니다.

부분적으로만 로드된 file-backed 데이터세트에서 `set_value(...)`는 대상 태그까지 자동으로 로드를 진행하지 않습니다.
대상 데이터 요소가 아직 파싱되지 않은 상태라면 `add_dataelement(...)`, `ensure_dataelement(...)`와 마찬가지로 예외가 발생합니다.

실패 모델:

- 성공하면 요청된 값이 기록됩니다.
- 실패 시 `set_value()`는 `False`를 반환합니다.
- `DataSet` / `DicomFile`는 계속 사용할 수 있습니다.
- 대상 요소 상태가 지정되지 않았으므로 이에 의존해서는 안 됩니다.

롤백 의미 체계가 필요한 경우 이전 값을 직접 유지하고 명시적으로 복원하세요.

### 길이가 0인 값 만들기 및 요소 제거

`None`은 현재 요소가 존재하지만 값 길이가 0임을 뜻합니다.

```python
assert ds.set_value("PatientName", None)   # present, zero-length PN
assert ds.set_value("Rows", None)          # present, zero-length US
```

`None`은 해석된 VR에 맞는 길이 0 표현을 간단히 적는 방법입니다.
명시적으로 빈 페이로드를 사용해 같은 의도를 나타낼 수도 있습니다.

```python
ds.add_dataelement(dicom.Tag("PatientName"), dicom.VR.PN)   # present, zero-length

assert ds.set_value("PatientName", "")      # zero-length text element
assert ds.set_value("Rows", [])             # zero-length numeric VM-based element
assert ds.set_value(0x00111001, dicom.VR.OB, b"")  # zero-length binary element
```

기존 요소에도 동일한 규칙이 적용됩니다.

```python
ds["PatientName"].value = None
ds["PatientName"].value = ""
ds["Rows"].value = None
ds["Rows"].value = []
```

권장 해석:

- `None` -> 현재 존재하는 길이 0 요소를 유지하거나 새로 만듭니다.
- 빈 페이로드(`""`, `[]`, `b""`) -> 요소를 유지하되 `length == 0`인 상태로 둡니다.

삭제하려면 `remove_dataelement()`를 사용하세요.

```python
ds.remove_dataelement("PatientName")
ds.remove_dataelement(0x00280010)
ds.remove_dataelement(dicom.Tag("Rows"))
```

### 프라이빗 태그 또는 모호한 태그에 대한 명시적 VR 할당

```python
assert ds.set_value(0x00090030, dicom.VR.US, 16)
```

이 형식은 태그가 프라이빗이거나, 값을 할당하기 전에 기존 비시퀀스 요소의 VR을 덮어써야 할 때 유용합니다.

규칙:

- 요소가 누락된 경우 제공된 VR을 사용하여 요소를 생성합니다.
- 요소가 존재하고 이미 해당 VR이 있는 경우 값이 그 자리에서 업데이트됩니다.
- 요소가 다른 비`SQ`/비`PX` VR로 존재하는 경우 바인딩은 그 VR을 교체한 뒤 값을 할당할 수 있습니다.
- `VR.None`, `SQ`, `PX`는 이 형식에서 유효한 덮어쓰기 대상이 아닙니다.

이 형식은 롤백 동작을 제공하지 않습니다. `False`를 반환하더라도 데이터세트는
유효한 상태로 유지되지만 대상 요소 상태가 지정되지 않습니다.

### 속성 할당 편의 경로

```python
ds.Rows = 512
df.PatientName = pn
```

속성 할당은 표준 키워드 기반 업데이트를 더 간단하게 쓰는 편의 문법입니다.
`DataSet`에서도, `DicomFile`을 통해 노출되는 동일한 접근 경로에서도 사용할 수 있습니다.
`set_value(...)`와 달리 이 경로는 `False`를 반환하지 않고, 할당 실패 시 예외를 발생시킵니다.
따라서 보통은 개발, 노트북, 대화형 사용에 더 적합하고,
명시적인 오류 처리가 필요한 프로덕션 코드에는 `set_value(...)`가 더 낫습니다.

## 유틸리티 작업

### 반복 및 크기

`for elem in ds`는 해당 데이터세트에 현재 존재하는 요소를 순회합니다.
`ds.size()`는 해당 데이터세트의 요소 개수를 반환합니다.
`len(df)`는 `DicomFile`의 루트 데이터세트 크기를 그대로 돌려줍니다.

```pycon
>>> for elem in ds:
...     print(elem.tag, elem.vr, elem.length)
(0002,0010) UI 20
(0010,0010) PN 8
(0028,0010) US 2
>>> ds.size()
42
>>> len(df)
42
```

이미 데이터세트 객체를 가지고 있다면 `ds.size()`를 사용하세요.
아직 파일 객체를 기준으로 작업 중이라면 `len(df)`를 사용하세요.

### dump()

`dump()`는 `DicomFile`과 `DataSet` 모두에서 사람이 읽기 쉬운 탭 구분 덤프 문자열을 반환합니다.

```python
full_text = df.dump(max_print_chars=80, include_offset=True)
compact_text = ds.dump(max_print_chars=40, include_offset=False)
```

- `max_print_chars`는 긴 `VALUE` 미리보기를 자릅니다.
- `include_offset=False`는 `OFFSET` 열을 제거합니다.
- 파일 기반 루트 데이터세트에서는 `dump()`가 덤프를 포맷하기 전에 아직 읽지 않은 나머지 요소도 불러옵니다.

대표적인 출력은 다음과 같습니다.

```text
TAG	VR	LEN	VM	OFFSET	VALUE	KEYWORD
'00020010'	UI	20	1	132	'1.2.840.10008.1.2.1'	TransferSyntaxUID
'00100010'	PN	8	1	340	'Doe^Jane'	PatientName
'00280010'	US	2	1	702	512	Rows
```

`include_offset=False`를 사용하면 헤더와 열은 다음과 같습니다.

```text
TAG	VR	LEN	VM	VALUE	KEYWORD
'00100010'	PN	8	1	'Doe^Jane'	PatientName
```

## 추가 참고사항

### 성능 노트

- 키워드 및 태그 조회는 상수 시간 사전 경로를 사용합니다.
- 대용량 파일의 경우 Python 핫 루프의 전체 반복보다 대상 요소 액세스를 선호합니다.

### 픽셀 변환 메타데이터

다음에 대한 프레임 인식 메타데이터 해상도:

- `DicomFile.rescale_transform_for_frame(frame_index)`
- `DicomFile.window_transform_for_frame(frame_index)`
- `DicomFile.voi_lut_for_frame(frame_index)`
- `DicomFile.modality_lut_for_frame(frame_index)`

자세한 내용은 [픽셀 변환 메타데이터 해상도](../reference/pixel_transform_metadata.md)에 설명되어 있습니다.

### 실행 가능한 예

- `examples/python/dataset_access_example.py`
- `examples/python/dump_dataset_example.py`

## 관련 문서

- C++ 대응: [C++ 데이터세트 가이드](cpp_dataset_guide.md)
- 입출력 동작: [파일 I/O](file_io.md)
- 파일 수준 API 표면: [DicomFile Reference](../reference/dicomfile_reference.md)
- `DataElement` 세부정보: [DataElement 참조](../reference/dataelement_reference.md)
- `Sequence` 순회: [시퀀스 참조](../reference/sequence_reference.md)
- 예외 및 실패 카테고리: [오류 처리](error_handling.md)
- 디코딩된 픽셀 출력: [픽셀 디코드](pixel_decode.md)
- 텍스트 VR 및 `PN`: [문자 집합 및 사람 이름](charset_and_person_name.md)
- Python 유형 지원: [Python API 참조](../reference/python_reference.md)
- UID 생성 관련 내용: [UID 생성](generating_uid.md)
- 픽셀 인코딩 제한: [픽셀 인코딩 제약 조건](../reference/pixel_encode_constraints.md)
