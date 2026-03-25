# 문자 세트 및 사람 이름

텍스트 VR 또는 `PN` 값이 `SpecificCharacterSet`에 종속되고 디코딩된 UTF-8 또는 구조화된 이름 구성 요소를 원하는 경우 `to_utf8_string()` / `to_person_name()`를 사용하세요. 문자 세트 디코딩 없이 일반 VR 트리밍 후에 의도적으로 저장된 바이트를 원하는 경우에만 `to_string_view()`를 사용하십시오. 문자 세트 인식 쓰기를 원할 경우 `from_utf8_view()` / `from_person_name()`를 사용하십시오. 현재 데이터 세트 하위 트리를 정규화하거나 새 문자 세트로 트랜스코딩하려는 경우 `set_specific_charset()`를 사용합니다. 이미 저장된 바이트에 대한 누락되거나 잘못된 선언을 복구해야 하는 경우 [문제 해결](troubleshooting.md)을 참조하세요.

범위 참고 사항: 아래의 읽기/쓰기 도우미 대부분은 `DataElement` 메서드입니다. `(0008,0005)`를 다시 쓰거나 다시 선언하는 문자 집합 돌연변이 API는 `DataSet` / `DicomFile`에 있습니다.

## 주요 문자 집합 및 PN API

**C++**

`DataElement` 방법

- `to_string_view()` / `to_string_views()`
  - 문자 세트 디코드 없이 잘린 원시 저장된 바이트를 읽습니다.
- `to_utf8_string()` / `to_utf8_strings()`
  - 문자 세트 디코딩 후 소유된 UTF-8로 텍스트 VR을 읽습니다.
- `to_person_name()` / `to_person_names()`
  - `PN` 값을 알파벳, 표의 문자 및 음성 그룹으로 구문 분석합니다.
- `from_utf8_view()` / `from_utf8_views()`
  - 소유 데이터 세트에 현재 선언된 문자 세트로 UTF-8 텍스트를 인코딩합니다.
- `from_person_name()` / `from_person_names()`
  - 구조화된 `PersonName` 값을 `PN` 요소로 직렬화합니다.

`DataSet` / `DicomFile` 방법

- `set_specific_charset()`
  - 기존 텍스트 바이트를 새 문자 세트로 트랜스코딩하고 `(0008,0005)`를 일관되게 업데이트합니다.

`Helper types`

- `PersonName` / `PersonNameGroup`
  - 수동 `^` 및 `=` 문자열 처리 없이 `PN` 값을 작성하거나 검사하기 위한 도우미 유형입니다.

**파이썬**

`DataElement` 방법

- `to_string_view()` / `to_string_views()`
  - 문자셋 디코드 없이 잘린 원시 저장된 텍스트를 읽습니다.
- `to_utf8_string()` / `to_utf8_strings()`
  - 텍스트 VR을 디코딩된 UTF-8 문자열로 읽습니다. `return_replaced=True`를 사용하면 디코드 대체 바이트가 대체되었는지 여부도 확인할 수 있습니다.
- `to_person_name()` / `to_person_names()`
  - 알파벳, 표의 문자 및 음성 그룹을 사용하여 `PN` 값을 `PersonName` 개체로 구문 분석합니다.
- `from_utf8_view()` / `from_utf8_views()`
  - Python `str` 데이터를 데이터 세트의 선언된 문자 세트로 인코딩합니다. `return_replaced=True`를 사용하면 교체 동작을 검사할 수 있습니다.
- `from_person_name()` / `from_person_names()`
  - `PersonName` 개체를 `PN` 요소로 직렬화합니다.

`DataSet` / `DicomFile` 방법

- `set_specific_charset()`
  - 기존 텍스트 값을 새 문자 세트로 트랜스코딩하고 `(0008,0005)`를 일관되게 다시 작성합니다.

`Helper types`

- `PersonName(...)` / `PersonNameGroup(...)`
  - Python 문자열 또는 튜플에서 직접 구조화된 `PN` 값을 구성합니다.

## 관련 DICOM 표준 섹션

- `Specific Character Set` 속성 자체는 [DICOM PS3.3 섹션 C.12, 일반 모듈](https://dicom.nema.org/medical/dicom/current/output/chtml/part03/sect_C.12.html)의 SOP 공통 모듈에 속합니다.
- Character repertoire selection, replacement, and ISO/IEC 2022 code extension behavior are defined in [DICOM PS3.5 Chapter 6, Value Encoding](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/chapter_6.html), especially Section 6.1 and Sections 6.1.2.4 through 6.1.2.5.
- `PN` 규칙은 [DICOM PS3.5 섹션 6.2, 값 표현](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/sect_6.2.html), 특히 섹션 6.2.1, 사람 이름(PN) 값 표현에 정의되어 있습니다.
- Language-specific examples for Japanese, Korean, Unicode UTF-8, GB18030, and GBK live in the informative [DICOM PS3.5 Annex H](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/chapter_H.html), [Annex I](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/chapter_I.html), and [Annex J](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/chapter_J.html).

## C++

### 저장된 원시 텍스트를 디코딩된 UTF-8과 비교

```cpp
#include <dicom.h>
#include <iostream>

using namespace dicom::literals;

auto file = dicom::read_file("patient_names.dcm");
const auto& patient_name = file->dataset()["PatientName"_tag];

// to_string_view()는 일반 VR 트리밍 후에만 저장한 텍스트 포인트를 제공합니다.
// 여기서는 특정 문자 세트 디코드가 발생하지 않습니다.
if (auto raw = patient_name.to_string_view()) {
    std::cout << "raw: " << *raw << '\n';
}

// to_utf8_string()는 데이터 세트가 선언한 특정 문자 집합에 따라 표시됩니다.
if (auto utf8 = patient_name.to_utf8_string()) {
    std::cout << "utf8: " << *utf8 << '\n';
}

// to_person_name()는 한 단계 더 노력하여 PN 그룹과 구성 요소를 분석합니다.
if (auto parsed = patient_name.to_person_name()) {
    if (parsed->alphabetic) {
        std::cout << parsed->alphabetic->family_name() << '\n';
        std::cout << parsed->alphabetic->given_name() << '\n';
    }
}
```

첫 번째 `PatientName` 값이 `Hong^Gildong=洪^吉洞=홍^길동`인 경우 출력 예:

```text
raw: Hong^Gildong=洪^吉洞=홍^길동
utf8: Hong^Gildong=洪^吉洞=홍^길동
Hong
Gildong
```

### 구조화된 PersonName 구축 및 저장

```cpp
#include <dicom.h>
#include <iostream>

using namespace dicom::literals;

dicom::DicomFile file;
file.set_specific_charset(dicom::SpecificCharacterSet::ISO_IR_192);

dicom::PersonName name;
name.alphabetic = dicom::PersonNameGroup{{"Hong", "Gildong", "", "", ""}};
name.ideographic = dicom::PersonNameGroup{{"洪", "吉洞", "", "", ""}};
name.phonetic = dicom::PersonNameGroup{{"홍", "길동", "", "", ""}};

auto& patient_name = file.add_dataelement("PatientName"_tag, dicom::VR::PN);
if (!patient_name.from_person_name(name)) {
    // from_person_name()도 false로 인해 실패를 보고합니다.
}

if (auto parsed = patient_name.to_person_name()) {
    std::cout << parsed->alphabetic->family_name() << '\n';
    std::cout << parsed->ideographic->family_name() << '\n';
    std::cout << parsed->phonetic->family_name() << '\n';
}
```

예상 출력:

```text
Hong
洪
홍
```

### 기존 텍스트 값을 새 문자 세트로 트랜스코딩

```cpp
#include <dicom.h>
#include <iostream>

using namespace dicom::literals;

auto file = dicom::read_file("utf8_names.dcm");
bool replaced = false;

// set_ 시리즈_charset()은 데이터 세트 하위 트리를 탐색하고 텍스트 VR 값을 다시 작성합니다.
// (0008,0005)를 새 선언으로 업데이트합니다. 이 정책은
// 이동하는 동안 트랜스코드된 문자에 대한 눈에 보이는 흔적을 남기면서
// 대상 문자셋은 직접적으로 표현할 수 없습니다.
file->set_specific_charset(
    dicom::SpecificCharacterSet::ISO_IR_100,
    dicom::CharsetEncodeErrorPolicy::replace_unicode_escape,
    &replaced);

// 다시 저장된 바이트는 이제 일반 ASCII 문자로 to_string_view()입니다.
// 및 to_utf8_string() 모두 여기에 동일한 표시 이스케이프 마커를 표시합니다.
if (auto raw_name = file->dataset()["PatientName"_tag].to_string_view()) {
    std::cout << *raw_name << '\n';
}
std::cout << std::boolalpha << replaced << '\n';
```

`utf8_names.dcm`에 `홍길동`가 포함된 경우 출력 예:

```text
(U+D64D)(U+AE38)(U+B3D9)
true
```

### 선언 및 트랜스코딩 실패를 명시적으로 처리

```cpp
#include <dicom.h>
#include <diagnostics.h>
#include <iostream>

using namespace dicom::literals;

try {
    dicom::DicomFile file;
    file.set_specific_charset(dicom::SpecificCharacterSet::ISO_IR_192);

    auto& patient_name = file.add_dataelement("PatientName"_tag, dicom::VR::PN);
    if (!patient_name.from_utf8_view("홍길동")) {
        std::cerr << "initial UTF-8 assignment failed\n";
    }

    // set_specific_charset()는 from_utf8_view()와 별개의 API입니다.
    // 데이터세트 전체 선언/트랜스코드 문제는 false를 반환하는 대신 예외를 발생시킵니다.
    file.set_specific_charset(
        dicom::SpecificCharacterSet::ISO_IR_100,
        dicom::CharsetEncodeErrorPolicy::strict);
} catch (const dicom::diag::DicomException& ex) {
    std::cerr << ex.what() << '\n';
}
```

## 파이썬

### 저장된 원시 텍스트를 디코딩된 UTF-8과 비교

```python
import dicomsdl as dicom

df = dicom.read_file("patient_names.dcm")
elem = df.dataset["PatientName"]

# to_string_view()는 일반 VR를 트리밍하고 나서만 남았습니다.
# 여기서는 특정 문자 세트 디코드가 발생하지 않습니다.
raw = elem.to_string_view()

# to_utf8_string()은(는) Python str 또는 없음을 반환합니다.
text, replaced = elem.to_utf8_string(return_replaced=True)

# to_person_name()은 구조화된 PersonName 또는 없음을 반환합니다.
name = elem.to_person_name()
if name is not None and name.alphabetic is not None:
    print(name.alphabetic.family_name)
    print(name.alphabetic.given_name)
```

첫 번째 `PatientName` 값이 `Hong^Gildong=洪^吉洞=홍^길동`인 경우 출력 예:

```text
Hong
Gildong
```

### 구조화된 PersonName 구축 및 저장

```python
import dicomsdl as dicom

df = dicom.DicomFile()
df.set_specific_charset("ISO_IR 192")

pn = dicom.PersonName(
    alphabetic=("Hong", "Gildong"),
    ideographic=("洪", "吉洞"),
    phonetic=("홍", "길동"),
)

patient_name = df.dataset.add_dataelement(dicom.Tag("PatientName"), dicom.VR.PN)
ok = patient_name.from_person_name(pn)

# 동일한 PersonName를 사용하여 데이터를 설정하고 함께 사용할 수도 있습니다.
df.PatientName = pn

value = df.PatientName
print(value.alphabetic.family_name)
print(value.ideographic.family_name)
print(value.phonetic.family_name)
```

예상 출력:

```text
Hong
洪
홍
```

### 기존 텍스트 값을 트랜스코딩하고 교체 검사

```python
import dicomsdl as dicom

df = dicom.read_file("utf8_names.dcm")

# 가시적인 폴백은 종종 엄격한 실패보다 작업하기가 더 쉽습니다.
# 트랜스코드가 완료되고 교체가 완료되었기 때문에 프로덕션 정리가 통과되었습니다.
# 결과 텍스트에서는 여전히 분명합니다.
replaced = df.set_specific_charset(
    "ISO_IR 100",
    errors="replace_unicode_escape",
    return_replaced=True,
)
print(df.get_dataelement("PatientName").to_string_view())
print(replaced)
```

`utf8_names.dcm`에 `홍길동`가 포함된 경우 예상되는 출력:

```text
(U+D64D)(U+AE38)(U+B3D9)
True
```

### 선언 및 트랜스코딩 실패를 명시적으로 처리

```python
import dicomsdl as dicom

df = dicom.DicomFile()

try:
    df.set_specific_charset("ISO_IR 192")
    patient_name = df.dataset.add_dataelement(dicom.Tag("PatientName"), dicom.VR.PN)
    ok = patient_name.from_utf8_view("홍길동")
    print(ok)

    # 선택한 오류 정책에서도 요청한 트랜스코드를 수행할 수 없다면
    # set_specific_charset()는 예외를 발생시킵니다.
    df.set_specific_charset("ISO_IR 100", errors="strict")
except (TypeError, ValueError) as exc:
    # 문자 집합 인수 형태가 잘못되었거나 정책 텍스트가 잘못되었습니다.
    print(exc)
except RuntimeError as exc:
    # 기본 선언 또는 트랜스코드 단계가 실패했습니다.
    print(exc)
```

## `set_specific_charset()` 정책 옵션

첫 번째 인수는 대상 문자 집합을 선택합니다. 두 번째 인수는 대상 문자 집합이 나타낼 수 없는 문자를 처리하는 방법을 선택합니다. 선택적 세 번째 출력은 실제로 대체가 발생했는지 여부를 보고하며, 이는 주로 손실 모드에 유용합니다.

모든 텍스트 값이 대상 문자 집합에서 표현 가능한 경우 모든 정책은 동일한 트랜스코딩된 데이터 세트를 생성하고 `replaced == false`를 보고합니다. 차이점은 일부 기존 텍스트를 요청된 대상 문자 집합에 표시할 수 없는 경우에만 중요합니다.

정책 이름은 두 API에 직접 매핑됩니다.

- C++: `dicom::CharsetEncodeErrorPolicy::strict`, `::replace_qmark`, `::replace_unicode_escape`
- 파이썬: `errors="strict"`, `"replace_qmark"`, `"replace_unicode_escape"`

예를 들어 소스 텍스트가 `홍길동`이고 대상 문자 집합이 `ISO_IR 100`이라면, 해당 문자 집합은 한글을 직접 표현할 수 없습니다. 이때 정책에 따라 결과가 다음과 같이 달라집니다.

| 비교점 | `strict` | `replace_qmark` | `replace_unicode_escape` |
| --- | --- | --- | --- |
| 일부 텍스트를 표현할 수 없는 경우 | `set_specific_charset()`가 예외를 던지고 중단합니다. | 트랜스코드는 성공하고 `?`로 치환됩니다. | 트랜스코드는 성공하고 `(U+XXXX)` 형태의 텍스트로 치환됩니다. |
| `홍길동 -> ISO_IR 100` 결과 예시 | 호출이 실패하므로 트랜스코딩된 텍스트가 생성되지 않습니다. | `???` | `(U+D64D)(U+AE38)(U+B3D9)` |
| 데이터세트에 반영되는 결과 | 변경되지 않습니다. | 문자 집합이 업데이트되고 텍스트 VR이 `?`로 다시 기록됩니다. | 문자 집합이 업데이트되고 텍스트 VR이 `(U+XXXX)` 치환 텍스트로 다시 기록됩니다. |
| `replaced` 출력 | 호출이 실패하므로 해당되지 않습니다. | 하나 이상 치환이 발생하면 `true`입니다. | 하나 이상 치환이 발생하면 `true`입니다. |

선택적 `replaced` 출력은 위의 손실 모드에서 가장 유용합니다.

- C++에서는 `bool* out_replaced`를 전달합니다.
- Python에서는 `return_replaced=True`를 사용합니다.
- 트랜스코드가 정확할 때 `false`를 유지하고 대체 정책이 실제로 문자를 대체해야 하는 경우에만 `true`로 전환됩니다.

또한 트랜스코드에는 대상 인코딩에 앞서 소스 디코드 단계가 있습니다. 현재 데이터세트에 현재 선언으로는 디코딩할 수 없는 바이트가 이미 들어 있다면, 같은 정책 이름이 이 단계에도 적용됩니다.

예를 들어, 현재 선언이 `ISO_IR 192`이지만 저장된 원시 텍스트 값에 잘못된 UTF-8 바이트 `b"\xFF"`가 포함되어 있는 경우 소스 디코드 단계는 다음과 같이 분기됩니다.

| 비교점 | `strict` | `replace_qmark` | `replace_unicode_escape` |
| --- | --- | --- | --- |
| 현재 저장된 바이트가 이미 디코딩 불가능한 경우 | `set_specific_charset()`가 예외를 던지고 중단합니다. | 트랜스코드는 계속 진행되며, 디코딩할 수 없는 소스 바이트 구간을 `?`로 치환합니다. | 트랜스코드는 계속 진행되며, 눈에 보이는 바이트 이스케이프로 치환합니다. |
| 원시 바이트 `b"\xFF"` 치환 예시 | 호출이 실패하므로 트랜스코딩된 텍스트가 생성되지 않습니다. | `?` | `(0xFF)` |
| 대상 인코딩 치환과 다른 이유 | 유니코드 텍스트를 복구하지 못했기 때문에 트랜스코드를 계속할 수 없습니다. | 유니코드 코드 포인트를 복구하지 못했기 때문에 치환 결과는 `?`뿐입니다. | 유니코드 코드 포인트를 복구하지 못했기 때문에 `(U+XXXX)` 대신 `(0xNN)` 바이트 이스케이프를 사용합니다. |

## `to_utf8_string()` 디코드 정책 옵션

이러한 정책은 현재 선언된 문자 집합에서 저장된 바이트를 깔끔하게 디코딩할 수 없을 때 발생하는 상황을 제어합니다.

정책 이름은 두 API에 직접 매핑됩니다.

- C++: `dicom::CharsetDecodeErrorPolicy::strict`, `::replace_fffd`, `::replace_hex_escape`
- 파이썬: `errors="strict"`, `"replace_fffd"`, `"replace_hex_escape"`

예를 들어 데이터세트가 `ISO 2022 IR 100`를 선언했지만 `b"\x1b%GA"`와 같이 저장된 원시 바이트가 해당 디코드 경로에 유효하지 않은 경우 `to_utf8_string()`는 다음과 같이 분기됩니다.

| 비교점 | `strict` | `replace_fffd` | `replace_hex_escape` |
| --- | --- | --- | --- |
| 저장된 바이트를 깔끔하게 디코딩할 수 없는 경우 | `to_utf8_string()`가 실패합니다. | 대체 문자로 디코딩에 성공합니다. | 보이는 바이트 이스케이프를 통해 디코드가 성공합니다. |
| `b"\x1b%GA"`의 결과 예 | 디코딩된 텍스트가 생성되지 않습니다. | `�` | `(0x1B)(0x25)(0x47)(0x41)` |
| 반환 값 | C++의 `nullopt`, Python의 `None` | 디코딩된 UTF-8 텍스트 | 디코딩된 UTF-8 텍스트 |
| `replaced` 출력 | 값이 반환되지 않으므로 `false` | 하나 이상의 교체가 발생한 경우 `true` | 하나 이상의 교체가 발생한 경우 `true` |

## `from_utf8_view()` 인코딩 정책 옵션

이 정책들은 입력 UTF-8 텍스트를 데이터세트의 현재 선언 문자 집합으로 표현할 수 없을 때의 동작을 제어합니다. `from_utf8_view()`는 반환값 기반 API이므로 `set_specific_charset()`와 달리 예외를 발생시키지 않고 `false`로 일반적인 인코딩 실패를 보고합니다.

정책 이름은 두 API에 직접 매핑됩니다.

- C++: `dicom::CharsetEncodeErrorPolicy::strict`, `::replace_qmark`, `::replace_unicode_escape`
- 파이썬: `errors="strict"`, `"replace_qmark"`, `"replace_unicode_escape"`

예를 들어 데이터셋이 `ISO_IR 100`으로 선언되어 있고 입력 텍스트가 `홍길동`이라면, 선언된 문자셋은 한글을 직접 표현할 수 없습니다. 이때 `from_utf8_view()`는 다음과 같이 분기됩니다.

| 비교점 | `strict` | `replace_qmark` | `replace_unicode_escape` |
| --- | --- | --- | --- |
| 선언된 문자 세트에서 입력 텍스트를 표현할 수 없는 경우 | 호출이 실패하고 새 내용은 저장되지 않습니다. | 호출이 성공하고 `?`로 치환됩니다. | 호출이 성공하고 `(U+XXXX)` 형태의 텍스트로 치환됩니다. |
| `홍길동 -> ISO_IR 100` 저장 결과 예시 | 인코딩된 텍스트가 생성되지 않습니다. | `???` | `(U+D64D)(U+AE38)(U+B3D9)` |
| 반환 값 | `false` | `true` | `true` |
| `replaced` 출력 | 쓰기가 성공하지 못했으므로 `false` | 하나 이상 치환이 발생하면 `true` | 하나 이상 치환이 발생하면 `true` |

## 실패 모델

**C++**

| API | 실패 형태 | 일반적인 이유 |
| --- | --- | --- |
| `to_utf8_string()` / `to_person_name()` | 빈 `std::optional` | 잘못된 VR, 문자 세트 디코드에 실패했거나 디코드 후 `PN` 구문을 구문 분석할 수 없습니다. |
| `from_utf8_view()` / `from_person_name()` | `false` | VR이 잘못되었습니다. 입력이 유효한 UTF-8이 아니거나, 선언된 문자 세트가 선택한 정책에 따라 텍스트를 나타낼 수 없거나, DICOM 이유로 할당이 실패했습니다. |
| `set_specific_charset()` | `dicom::diag::DicomException` | 잘못된 대상 문자 집합 선언, 지원되지 않는 선언 조합, 또는 데이터세트 전체 트랜스코드 오류 |

**파이썬**

| API | 실패 형태 | 일반적인 이유 |
| --- | --- | --- |
| `to_utf8_string()` / `to_person_name()` | `None` 또는 `(None, replaced)` | 잘못된 VR, 문자 세트 디코드에 실패했거나 디코드 후 `PN` 구문을 구문 분석할 수 없습니다. 잘못된 `errors=` 값은 `ValueError`를 발생시킵니다. |
| `from_utf8_view()` / `from_person_name()` | `False` 또는 `(False, replaced)` | 대상 VR이 호환되지 않거나, 선언된 문자 집합으로 선택한 정책의 텍스트를 표현할 수 없거나, 할당이 실패했습니다. 잘못된 Python 인수 유형이면 `TypeError`가 발생합니다. |
| `set_specific_charset()` | `TypeError`, `ValueError`, `RuntimeError` | charset 인수 형태가 잘못되었거나, charset 용어를 알 수 없거나, 기본 C++ 트랜스코드 단계가 실패합니다. |
| `PersonNameGroup.component(index)` | `IndexError` | 구성요소 색인이 `[0, 4]` 외부에 있습니다. |

## 메모

- `to_string_view()` 및 `to_string_views()`는 VR 트리밍 규칙 후에 원시 텍스트 바이트를 반환합니다. 문자셋 디코드를 수행하지 않습니다. 응용프로그램에 표시되는 텍스트에는 `to_utf8_string()` 및 `to_utf8_strings()`를 사용하십시오.
- `to_string_views()`는 ISO 2022 JIS, GBK 또는 GB18030과 같은 선언된 멀티바이트 문자 세트에 대해 `nullopt` / `None`를 반환할 수 있습니다. 왜냐하면 `\`에서 원시 바이트를 분할하는 것은 디코딩 전에 안전하지 않기 때문입니다.
- `set_specific_charset()`는 데이터세트 하위 트리의 텍스트 VR 값을 다시 쓰고 `(0008,0005)`를 새 선언과 동기화합니다.
- `set_specific_charset("ISO_IR 192")`는 나중에 `from_utf8_view()` 또는 `from_person_name()`가 쓰기 전에 데이터세트를 UTF-8 선언 상태로 유지하므로 새로운 유니코드 콘텐츠에 대한 합리적인 일반 흐름 시작점입니다.
- `from_utf8_view()`와 `from_person_name()`는 일반적인 반환값 기반 API입니다. `false`는 요소 쓰기가 성공하지 못했음을 의미합니다. `set_specific_charset()`는 유효성 검사/트랜스코드 API이며, 실패 시 예외를 발생시킵니다.
- `PersonName`는 알파벳, 표의 문자, 음성 문자 등 최대 3개의 그룹을 전달합니다.
- `PersonNameGroup`는 DICOM 순서로 성, 이름, 중간 이름, 접두사, 접미사 등 최대 5개의 구성 요소를 전달합니다.
- 중첩된 시퀀스 항목 데이터세트는 해당 항목이 자체 로컬 `(0008,0005)`를 선언하지 않는 한 상위 항목으로부터 유효한 문자 세트를 상속합니다.
- `PersonName` 구문 분석 및 직렬화는 명시적인 빈 그룹과 빈 구성 요소를 보존하므로 해당 세부 정보를 유지하기 위해 `=` 및 `^` 구분 기호를 직접 조립할 필요가 없습니다.
- 새로운 유니코드 콘텐츠의 경우 `ISO_IR 192`는 일반적으로 저장된 텍스트가 ISO 2022 이스케이프 상태 관리가 없는 일반 UTF-8이기 때문에 가장 간단한 선언입니다.
- 저장된 바이트가 이미 정확하지만 `(0008,0005)`가 누락되었거나 잘못된 경우 선언 복구 경로는 [문제 해결](troubleshooting.md)을 참조하세요.
- 목표가 일반적인 트랜스코드 또는 정규화 흐름인 경우 `(0008,0005)`를 원시 요소로 변경하는 것보다 `set_specific_charset()`를 선호합니다.

## 관련 문서

- [핵심 개체](core_objects.md)
- [Python 데이터세트 가이드](python_dataset_guide.md)
- [C++ 데이터세트 가이드](cpp_dataset_guide.md)
- [오류 처리](error_handling.md)
- [문제 해결](troubleshooting.md)
