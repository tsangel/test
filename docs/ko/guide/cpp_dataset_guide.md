# C++ 데이터세트 가이드

이 문서는 DicomSDL에서 C++로 데이터세트와 요소를 다루는 주요 사용자 가이드입니다.
주요 객체의 관계, 태그를 표기하는 방법, 그리고 가장 중요한 읽기/쓰기 패턴을 다룹니다.

## DicomSDL이 DICOM에 매핑되는 방법

DicomSDL은 관련된 C++ 개체의 작은 집합을 노출합니다.

- `DicomFile`: 루트 데이터세트를 소유하는 파일/세션 래퍼
- `DataSet`: DICOM `DataElement` 개체의 컨테이너
- `DataElement`: 태그 / VR / 길이 메타데이터와 타입 변환 / 원시 값 접근을 제공하는 하나의 DICOM 필드
- `Sequence`: `SQ` 값에 대한 중첩 항목 컨테이너
- `PixelSequence`: 캡슐화되거나 압축된 픽셀 데이터를 위한 프레임/조각 컨테이너

개체 모델 및 DICOM 매핑은 [핵심 개체](core_objects.md)를 참조하세요.

C++에는 Python 스타일의 속성 편의 접근이 없습니다.
태그, 키워드, 점으로 구분된 태그 경로는 모두 명시적으로 적습니다.

### DicomFile 및 데이터세트

대부분의 데이터 요소 액세스 API는 `DataSet`에서 구현됩니다.
`DicomFile`는 루트 `DataSet`를 소유하고 로드, 저장, 트랜스코드와 같은 파일 지향 작업을 처리합니다.
편의를 위해 `DicomFile`는 `get_value(...)` 같은 루트 데이터세트 도우미를 많이 전달합니다.
`get_dataelement(...)`, `set_value(...)`, `ensure_dataelement(...)` 및 `ensure_loaded(...)`.

파일 수준 작업과 혼합된 몇 가지 루트 수준 읽기의 경우 `DicomFile` 전달로 충분할 때가 많습니다.

```cpp
#include <dicom.h>
using namespace dicom::literals;

auto file = dicom::read_file("sample.dcm");

long rows1 = file->get_value<long>("Rows"_tag, 0L);
const auto& patient_name1 = file->get_dataelement("PatientName"_tag);
```

반복되는 데이터 세트 작업의 경우 일반적으로 `DataSet`를 명시적으로 사용하는 것이 더 명확합니다.

```cpp
#include <dicom.h>
using namespace dicom::literals;

auto file = dicom::read_file("sample.dcm");
auto& ds = file->dataset();

long rows2 = ds.get_value<long>("Rows"_tag, 0L);
const auto& patient_name2 = ds["PatientName"_tag];
```

## 권장 API

| API | 반환값 | 누락 시 동작 | 용도 |
| --- | --- | --- | --- |
| `ds.get_value<long>("Rows"_tag)` | `std::optional<long>` | `std::nullopt` | `std::nullopt`로 누락을 구분하는 타입 읽기 |
| `ds.get_value<long>("Rows"_tag, 0L)` | `long` | 전달한 기본값을 반환 | 한 번에 끝나는 타입 읽기 |
| `ds["Rows"_tag]`, `ds["Rows"]`, `ds.get_dataelement("Rows")` | `DataElement&` | `VR::None`이며 `false`로 평가되는 요소를 반환 | 타입 읽기 + 메타데이터 접근 |
| `if (const auto& e = ds["Rows"_tag]; e)` | 존재 여부에 따라 분기 | 놓치면 `false` | 존재 여부를 구분해야 하는 코드 |
| `ds.ensure_loaded("(0028,FFFF)"_tag)` | `void` | 잘못된 사용 시 예외 발생 | 더 뒤쪽 태그 경계까지 부분 읽기를 명시적으로 계속 |
| `ds.ensure_dataelement("Rows"_tag, dicom::VR::US)` | `DataElement&` | 기존 요소를 반환하거나 새로 삽입 | 체이닝에 적합한 ensure/create |
| `ds.set_value("Rows"_tag, 512L)` | `bool` | 인코딩/할당 실패 시 `false` | 원샷 ensure + 타입 할당 |
| `ds.add_dataelement("Rows"_tag, dicom::VR::US)` | `DataElement&` | 생성/교체 | 명시적인 리프 삽입 |

## C++에서 태그를 표기하는 방법

사용자 정의 리터럴 접미사는 `"_Tag"`가 아니라 소문자로 `"_tag"`입니다.

| 방식 | 예시 | 먼저 고려할 상황 |
| --- | --- | --- |
| 키워드 리터럴 | `"Rows"_tag` | 대부분의 일반적인 C++ 코드에서 표준 태그를 쓸 때 |
| 숫자 태그 리터럴 | `"(0028,0010)"_tag` | 숫자 태그 표기가 가장 분명할 때 |
| 그룹/요소 생성자 | `dicom::Tag(0x0028, 0x0010)` | 그룹과 요소가 이미 별도 값으로 있을 때 |
| 패킹 태그 생성자 | `dicom::Tag(0x00280010)` | 태그가 이미 `0xGGGGEEEE` 형태의 패킹 값으로 있을 때 |
| 런타임 텍스트 파싱 | `dicom::Tag("Rows")`, `dicom::Tag("(0028,0010)")` | 키워드나 숫자 태그가 런타임 문자열로 들어올 때 |
| 문자열/경로 방식 | `ds["Rows"]`, `ds.get_value<double>("00540016.0.00181074")` | 키워드 조회나 중첩 경로 읽기/쓰기를 한 번에 하고 싶을 때 |

### `"Rows"_tag`

- 대부분의 표준 태그를 다루는 일반적인 C++ 코드에서 기본 선택으로 쓰기 좋습니다.
- 장점: 짧고 읽기 쉽고, 컴파일 타임에 확인되며, 런타임 문자열 파싱이 없습니다.
- 트레이드오프: 컴파일 타임에 고정된 문자열 리터럴에서만 쓸 수 있습니다.

### `"(0028,0010)"_tag`

- 숫자 태그 표기가 키워드보다 더 분명할 때 쓰기 좋습니다.
- 장점: 의미가 분명하고, 컴파일 타임에 확인되며, 태그 전용 API에서도 바로 쓸 수 있습니다.
- 트레이드오프: 키워드보다 장황하고 오타를 내기 쉽습니다.

### `dicom::Tag(0x0028, 0x0010)`

- 그룹과 요소가 이미 별도의 런타임 값으로 있을 때 쓰기 좋습니다.
- 장점: 명시적이고, 런타임 값과 잘 맞고, 텍스트 파싱이 없습니다.
- 트레이드오프: 키워드 리터럴보다 더 장황합니다.

### `dicom::Tag(0x00280010)`

- 태그가 이미 `0xGGGGEEEE` 형태의 패킹 값으로 있을 때 쓰기 좋습니다.
- 장점: 생성된 테이블이나 패킹된 태그 값과 연동할 때 간결합니다.
- 트레이드오프: bare `0x00280010` 자체는 `DataSet` API에서 받지 않으므로 `Tag(...)` 또는 `Tag::from_value(...)`로 감싸야 합니다.

### `dicom::Tag("Rows")` 또는 `dicom::Tag("(0028,0010)")`

- 키워드나 숫자 태그가 런타임 문자열로 들어올 때 쓰기 좋습니다.
- 장점: 키워드 문자열과 숫자 태그 문자열을 모두 받을 수 있습니다.
- 트레이드오프: 런타임에 문자열을 파싱하며, 잘못된 텍스트면 예외가 날 수 있습니다.

### 문자열/경로 방식

- 키워드 조회, 점으로 구분된 태그 경로 순회, one-shot 중첩 읽기/쓰기를 원할 때 쓰기 좋습니다.
- 예: `ds["Rows"]`, `ds.get_value<double>("00540016.0.00181074")`, `ds.set_value("PatientName", "Doe^Jane")`, `ds.ensure_dataelement("ReferencedStudySequence.0.ReferencedSOPInstanceUID", dicom::VR::UI)`.
- 장점: `Tag`를 명시적으로 만들지 않아도 되고, `operator[]`, `get_dataelement(...)`, `get_value(...)`, `set_value(...)`, `ensure_dataelement(...)`, `add_dataelement(...)` 전반에서 중첩 경로를 지원합니다.
- 트레이드오프: 문자열 파싱이 런타임에 일어납니다.

실용적인 권장:

- 대부분의 C++ 코드에서 일반적인 태그 액세스에는 `"Rows"_tag`를 사용합니다.
- 숫자 태그 표기가 가장 명확한 경우 `"(0028,0010)"_tag`를 사용합니다.
- 태그가 런타임 정수 또는 패킹된 값에서 나오는 경우 `dicom::Tag(...)`를 사용합니다.
- 중첩 조회를 한 번에 처리하거나 한 단계로 쓰고 싶다면 문자열/경로 방식을 사용합니다(예: `ds.get_value<double>("RadiopharmaceuticalInformationSequence.0.RadionuclideTotalDose")`, `ds.get_value<double>("00540016.0.00181074")`, `ds.set_value("ReferencedStudySequence.0.ReferencedSOPInstanceUID", "1.2.3")`, `ds.ensure_dataelement("ReferencedStudySequence.0.ReferencedSOPInstanceUID", dicom::VR::UI)`).

## 값 읽기

### 빠른 경로: get_value<T>()

타입 값만 필요할 때는 `get_value<T>()`를 사용하세요.

```cpp
#include <dicom.h>
using namespace dicom::literals;

auto file = dicom::read_file("sample.dcm");
auto& ds = file->dataset();

if (auto rows = ds.get_value<long>("Rows"_tag)) {
  // *rows를 사용하세요
}

double slope = ds.get_value<double>("RescaleSlope"_tag, 1.0);
auto desc = ds.get_value<std::string_view>("StudyDescription"_tag);
```

- `get_value<T>(tag)`는 `std::optional<T>`를 반환합니다. 호출자에서 "실제 값이 있음"과 "누락"을 구별하려는 경우 이를 사용하십시오.
- `get_value<T>(tag, default_value)`는 `T`를 반환합니다. 인라인 대체를 원하고 대체 경로와 빈 결과를 구별할 필요가 없는 경우 이를 사용합니다.
- 기본값 형태는 사실상 `get_value<T>(...).value_or(default_value)`입니다.
- `get_value<std::string_view>(...)`는 제로카피 뷰입니다. 소유한 데이터세트/파일을 사용하는 동안 활성 상태로 유지하세요.

### DataElement 액세스: 연산자[]

값뿐만 아니라 `DataElement`를 원할 때 `operator[]`를 사용하십시오.

```cpp
#include <dicom.h>
using namespace dicom::literals;

auto file = dicom::read_file("sample.dcm");
auto& ds = file->dataset();

const auto& rows_elem = ds["Rows"];
if (rows_elem) {
  long rows = rows_elem.to_long(0L);
}

const auto& dose_elem =
    ds["RadiopharmaceuticalInformationSequence.0.RadionuclideTotalDose"];
```

`operator[]`는 `Tag`, 키워드 문자열 또는 점으로 구분된 태그 경로를 허용합니다.
런타임 구문 분석 없이 컴파일 타임 태그 표기를 원할 경우 `"_tag"`를 사용하세요.

### 존재 여부 확인

존재 자체가 중요한 경우 반환된 `DataElement`의 참/거짓 평가를 사용합니다.

```cpp
if (const auto& rows_elem = ds["Rows"_tag]; rows_elem) {
  long rows = rows_elem.to_long(0L);
}

if (const auto& patient_name = ds.get_dataelement("PatientName"); patient_name) {
  // 존재하는 요소
}
```

누락된 조회는 예외를 발생시키는 대신 `VR::None`이며 `false`로 평가되는 요소를 반환합니다.

### 메서드 형태의 같은 조회: get_dataelement(...)

`get_dataelement(...)`는 `operator[]`와 동일한 조회를 수행합니다. 일부 코드베이스는 이름이 있는 함수가 `ds[...]`보다 더 명확하게 읽힐 때 메서드 형태를 선호합니다.

```cpp
const auto& dose = ds.get_dataelement(
    "RadiopharmaceuticalInformationSequence.0.RadionuclideTotalDose");
if (dose) {
  double value = dose.to_double(0.0);
}
```

### 부분 로드 지속

지금 필요한 태그 이전에서 부분 읽기가 멈췄다면 `ensure_loaded(tag)`를 사용하세요.

```cpp
ds.ensure_loaded("(0028,FFFF)"_tag);

long rows = ds.get_value<long>("Rows"_tag, 0L);
long cols = ds.get_value<long>("Columns"_tag, 0L);
```

`ensure_loaded(...)`는 `"Rows"_tag`, `"(0028,FFFF)"_tag` 또는 `dicom::Tag(0x0028, 0x0010)`와 같은 `Tag`를 사용합니다.
키워드 문자열이나 점으로 구분된 태그 경로를 사용하지 않습니다.

### 값 유형 및 길이가 0인 동작

주요 유형의 읽기 계열은 다음과 같습니다.

- 스칼라 숫자: `to_int()`, `to_long()`, `to_longlong()`, `to_double()`
- 벡터 숫자/태그: `to_longlong_vector()`, `to_double_vector()`, `to_tag_vector()`
- 텍스트: `to_string_view()`, `to_string_views()`, `to_utf8_string()`, `to_utf8_strings()`
- 태그 및 UID: `to_tag()`, `to_uid_string()`, `to_transfer_syntax_uid()`
- 사람 이름 : `to_person_name()`, `to_person_names()`

벡터 접근자의 경우 길이가 0인 값은 `std::nullopt`가 아닌 연결된 빈 컨테이너를 반환합니다.

```cpp
auto rows = ds["Rows"_tag].to_longlong_vector();         // empty vector when zero-length
auto wc = ds["WindowCenter"_tag].to_double_vector();     // empty vector when zero-length
auto at = ds["FrameIncrementPointer"_tag].to_tag_vector();
```

스칼라 접근자의 경우 `std::nullopt`를 "이 접근자로는 값을 얻을 수 없음"으로 해석하세요.
누락된 값과 길이가 0인 존재 요소를 구분해야 한다면 `DataElement` 자체를 확인해야 합니다.

### 길이가 0인 것과 누락된 것을 구별하기

DicomSDL에서 `missing` 및 `zero-length`는 서로 다른 요소 상태이므로 `DataElement` 수준에서 테스트해야 합니다.

```cpp
const auto& elem = ds["PatientName"_tag];

if (!elem) {
  // 누락된 조회
} else if (elem.length() == 0) {
  // 길이가 0인 존재 요소
} else {
  // 비어 있지 않은 값을 가진 존재 요소
}
```

실질적인 차이점:

- 누락된 요소
- `bool(elem) == false`
- `elem.is_missing() == true`
- `elem.vr() == dicom::VR::None`
- 길이가 0인 존재 요소
- `bool(elem) == true`
- `elem.is_missing() == false`
- `elem.vr() != dicom::VR::None`
- `elem.length() == 0`

## 데이터요소

`DataElement`는 주요 메타데이터를 포함하는 개체입니다.

### 핵심 속성

- `elem.tag()`
- `elem.vr()`
- `elem.length()`
- `elem.offset()`
- `elem.vm()`
- `elem.parent()`

```cpp
const auto& elem = ds["Rows"_tag];
auto tag = elem.tag();
auto vr = elem.vr();
auto length = elem.length();
```

### 참/거짓 평가와 누락 요소 객체

```cpp
const auto& elem = ds["PatientName"_tag];
if (elem) {
  // 존재하는 요소
}

const auto& missing = ds["NotARealKeyword"];
if (!missing && missing.is_missing()) {
  // 누락된 조회
}
```

길이가 0인 요소도 여전히 `bool(elem) == true`입니다. 이런 경우는 `elem.length() == 0`로 확인하세요.

### 타입별 읽기/쓰기 도우미

- `to_long()`, `to_double()`, `to_tag()`, `to_uid_string()`
- `to_string_view()`, `to_utf8_string()`, `to_utf8_strings()`
- `to_person_name()`, `to_person_names()`
- `from_long(...)`, `from_double(...)`, `from_tag(...)`
- `from_string_view(...)`, `from_utf8_view(...)`, `from_uid_string(...)`
- `from_person_name(...)`, `from_person_names(...)`

이미 `DataElement&`가 있다면 `from_xxx(...)` 도우미가 가장 직접적인 쓰기 경로입니다.

### 컨테이너 도우미

`SQ` 및 캡슐화된 픽셀 데이터의 경우:

- `elem.sequence()` / `elem.as_sequence()`
- `elem.pixel_sequence()` / `elem.as_pixel_sequence()`

이를 스칼라 문자열이나 숫자가 아닌 컨테이너 값으로 처리합니다.

### 원시 바이트 및 보기 수명

`value_span()`는 복사하지 않고 `std::span<const std::uint8_t>`를 반환합니다.

```cpp
const auto& pixel_data = ds["PixelData"_tag];
auto bytes = pixel_data.value_span();
// bytes.data(), bytes.size()
```

`to_string_view()` 계열 접근자도 뷰 기반입니다.
요소가 교체되거나 수정되면 뷰가 무효가 되므로, 소유한 데이터세트/파일을 살아 있게 유지하고 쓰기 후에는 뷰를 다시 가져오세요.

## 값 쓰기

### ensure_dataelement(...)

체이닝에 적합한 ensure/create 동작이 필요할 때는 `ensure_dataelement(...)`를 사용하세요.

```cpp
auto& existing_rows = ds.ensure_dataelement("Rows"_tag);  // default vr == VR::None
ds.ensure_dataelement("Rows"_tag, dicom::VR::US).from_long(512);
ds.ensure_dataelement(
    "ReferencedStudySequence.0.ReferencedSOPInstanceUID",
    dicom::VR::UI).from_uid_string("1.2.3");
```

규칙:

- 기존 요소 + 생략된 `vr`(기본값 `VR::None`) 또는 명시적 `VR::None` -> 있는 그대로 유지
- 기존 요소 + 명시적으로 다른 VR -> 제자리에 재설정
- 누락된 요소 + 명시적 VR -> 해당 VR에 길이가 0인 요소 삽입
- 표준 태그 누락 + 생략된 `vr` -> 사전 VR에 길이가 0인 요소 삽입
- 알 수 없는/개인 태그 누락 + `vr` 생략 -> 해결할 사전 VR이 없기 때문에 예외가 발생

### 반환된 DataElement를 통해 기존 요소 업데이트

요소가 이미 있는 경우 `DataElement`에서 `from_xxx(...)`를 사용하세요.

```cpp
if (auto& rows = ds["Rows"_tag]; rows) {
  rows.from_long(512);
}
```

생성 또는 업데이트 동작을 원하는 경우 `operator[]` 대신 `ensure_dataelement(...)`에서 시작하세요.

### set_value(...)를 사용한 일회성 할당

한 번의 호출로 같은 ensure + 타입 쓰기 흐름을 원할 때는 `set_value(...)`를 사용하세요.

```cpp
bool ok = true;
ok &= ds.set_value("Rows"_tag, 512L);
ok &= ds.set_value("Columns"_tag, 512L);
ok &= ds.set_value("BitsAllocated"_tag, 16L);
ok &= ds.set_value(dicom::Tag(0x0009, 0x0030), dicom::VR::US, 16L);  // private tag
```

이 함수는 위의 `ensure_dataelement(...)` 규칙을 그대로 따릅니다. 즉 기존 요소/누락 요소 처리와
명시적 `vr` / 생략된 `vr` 규칙이 동일하게 적용된 뒤, 인코딩이나 할당에 실패하면 `false`를 반환합니다.

실패 모델:

- 성공하면 요청된 값이 기록됩니다.
- 실패 시 `set_value()`는 `false`를 반환합니다.
- `DataSet` / `DicomFile`는 계속 사용할 수 있습니다.
- 대상 요소 상태가 지정되지 않았으므로 이에 의존해서는 안 됩니다.

롤백 의미 체계가 필요한 경우 이전 값을 직접 유지하고 명시적으로 복원하세요.

### 길이가 0인 값 만들기 및 요소 제거

길이가 0인 것과 제거는 다른 작업입니다.

`add_dataelement(...)`, `ensure_dataelement(...)`, 또는 명시적으로 빈 페이로드를 사용하면
길이가 0인 요소를 만들거나 유지할 수 있습니다:

```cpp
ds.add_dataelement("PatientName"_tag, dicom::VR::PN);  // present element with zero-length value
ds.set_value("PatientName"_tag, std::string_view{});

std::vector<long long> empty_numbers;
ds.set_value("Rows"_tag, std::span<const long long>(empty_numbers));
```

삭제하려면 `remove_dataelement(...)`를 사용하세요.

```cpp
ds.remove_dataelement("PatientName"_tag);
ds.remove_dataelement(dicom::Tag(0x0028, 0x0010));
```

### 비공개 또는 모호한 태그에 대한 명시적 VR 할당

```cpp
bool ok = ds.set_value(dicom::Tag(0x0009, 0x0030), dicom::VR::US, 16L);
```

이 형태는 태그가 private 이거나, 값을 할당하기 전에 기존 비시퀀스 요소의 VR을 바꾸고 싶을 때 유용합니다.

규칙:

- 요소가 누락된 경우 제공된 VR을 사용하여 요소를 생성합니다.
- 요소가 존재하고 이미 해당 VR이 있는 경우 값이 그 자리에서 업데이트됩니다.
- 요소가 다른 비`SQ`/비`PX` VR과 함께 존재하는 경우 바인딩은 해당 VR을 대체한 다음 값을 할당할 수 있습니다.
- `VR::None`, `SQ` 및 `PX`는 이 오버로드에 대한 유효한 재정의 대상이 아닙니다.

### add_dataelement(...)

명시적인 생성/바꾸기 의미를 원할 경우 `add_dataelement(...)`를 사용하세요.

```cpp
ds.add_dataelement("Rows"_tag, dicom::VR::US).from_long(512);
```

`ensure_dataelement(...)`와 비교하여 `add_dataelement(...)`는 기존 요소에 대해 더 파괴적입니다.
대상 요소가 이미 존재하는 경우 `add_dataelement(...)`는 이를 다시 채우기 전에 길이가 0인 새로운 요소로 재설정합니다.

명시적인 교체 동작이 원하는 경우 `add_dataelement(...)`를 사용하세요.
명시적인 VR 변경이 필요하지 않은 한 기존 요소를 유지하려면 대신 `ensure_dataelement(...)`를 사용하세요.
`set_value(...)`는 `add_dataelement(...)` 경로가 아닌 `ensure_dataelement(...)` 경로를 따릅니다.

## 유틸리티 작업

### 반복 및 크기

`for (const auto& elem : ds)`는 해당 데이터세트에 들어 있는 요소를 순회합니다.
`ds.size()`는 해당 데이터세트의 요소 개수를 반환합니다.
`file->size()`는 `DicomFile`에서 루트 데이터세트의 크기를 그대로 전달합니다.

```cpp
for (const auto& elem : ds) {
  std::cout << elem.tag().to_string()
            << ' ' << elem.vr().str()
            << ' ' << elem.length() << '\n';
}

std::cout << "element count: " << ds.size() << '\n';
std::cout << "file count: " << file->size() << '\n';
```

대표적인 출력은 다음과 같습니다.

```text
(0002,0010) UI 20
(0010,0010) PN 8
(0028,0010) US 2
element count: 42
file count: 42
```

### dump()

`dump()`는 `DataSet` 및 `DicomFile` 모두에서 사람이 읽을 수 있는 탭으로 구분된 덤프 문자열을 반환합니다.

```cpp
auto full = file->dump(80, true);
auto compact = ds.dump(40, false);
```

- `max_print_chars`는 긴 `VALUE` 미리보기를 잘라냅니다.
- `include_offset = false`는 `OFFSET` 열을 제거합니다.
- 파일 기반 루트 데이터세트에서는 `dump()`가 출력 전에 아직 읽지 않은 나머지 요소도 로드합니다.

대표적인 출력은 다음과 같습니다.

```text
TAG	VR	LEN	VM	OFFSET	VALUE	KEYWORD
'00020010'	UI	20	1	132	'1.2.840.10008.1.2.1'	TransferSyntaxUID
'00100010'	PN	8	1	340	'Doe^Jane'	PatientName
'00280010'	US	2	1	702	512	Rows
```

`include_offset = false`를 사용하면 헤더와 열은 다음과 같습니다.

```text
TAG	VR	LEN	VM	VALUE	KEYWORD
'00100010'	PN	8	1	'Doe^Jane'	PatientName
```

## 부분 로드 규칙

- `get_value(...)`, `get_dataelement(...)`, `operator[]`는 암시적으로 부분 로드를 계속하지 않습니다.
- 아직 구문 분석되지 않은 데이터 요소는 `ensure_loaded(tag)`를 호출할 때까지 누락된 것으로 동작합니다.
- `add_dataelement(...)`, `ensure_dataelement(...)`, `set_value(...)`는 대상 데이터 요소가 아직 파싱되지 않았다면 예외를 발생시킵니다.
- staged read 뒤에 더 뒤쪽 태그가 필요해지면, 먼저 로드 경계를 명시적으로 옮긴 다음 읽거나 쓰세요.

## 추가 참고사항

### 성능 노트

- 핫 경로의 일반적인 단일 태그 액세스에는 런타임 텍스트 구문 분석보다 `"_tag"` 리터럴 또는 재사용된 `dicom::Tag` 객체를 선호합니다.
- 타입 결과만 필요하고 `DataElement` 메타데이터가 필요하지 않다면 `get_value<T>(tag, default)`를 우선 고려하세요.
- 중첩 접근을 더 명확하게 쓰고 싶다면 문자열/경로 방식을 사용하세요. 반복되는 핫 루프 조회라면 태그를 캐시하거나 순회를 명시적으로 나누는 편이 좋습니다.

### 실행 가능한 예

- `examples/dataset_access_example.cpp`
- `examples/batch_assign_with_error_check.cpp`
- `examples/dump_dataset_example.cpp`
- `examples/tag_lookup_example.cpp`
- `examples/keyword_lookup_example.cpp`

## 관련 문서

- [핵심 개체](core_objects.md)
- [파일 I/O](file_io.md)
- [시퀀스 및 경로](sequence_and_paths.md)
- [Python 데이터세트 가이드](python_dataset_guide.md)
- [C++ API 개요](../reference/cpp_api.md)
- [데이터세트 참조](../reference/dataset_reference.md)
- [데이터 요소 참조](../reference/dataelement_reference.md)
- [오류 처리](error_handling.md)
