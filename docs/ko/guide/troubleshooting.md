# 문제 해결

빌드, 읽기, 디코드, 쓰기 중 첫 시도가 실패했을 때 가능한 원인을 가장 빠르게 찾고 싶다면 이 페이지를 보세요.

## 일반적인 실패 패턴

- 컴파일 전에 휠 빌드가 실패합니다.
Python, `pip`, `cmake`, 컴파일러 툴체인 및 활성 가상 환경을 확인하세요.
- 부분적으로 로드된 파일에서 이후 태그를 변경하려고 하면 예외가 발생합니다.
먼저 파일을 더 읽어 오거나, 아직 파싱되지 않은 데이터 요소는 변경하지 마세요.
- `decode_into()`가 배열 shape, dtype 또는 버퍼 크기 불일치를 보고합니다.
행, 열, 픽셀당 샘플, 프레임 수 및 출력 항목 크기를 다시 확인하세요.
- 문자 집합 재작성이 실패하거나 교체가 발생합니다.
선언된 대상 문자 집합을 검토하고 오류 정책을 인코딩합니다.
- 태그/경로 조회가 해결되지 않음:
키워드 철자나 점선 경로 형식을 확인하세요.

## Charset 선언 복구

저장된 텍스트 바이트가 이미 정확하지만 `(0008,0005) Specific Character Set`가 누락되었거나 잘못된 경우에만 이 경로를 사용하십시오. 이 경우 기본 바이트가 양호하더라도 `to_utf8_string()` 또는 `to_person_name()`가 실패할 수 있습니다.

이 경로를 일반적인 트랜스코드 작업 흐름으로 사용하지 마십시오. 텍스트를 다른 문자 세트로 다시 작성해야 하는 경우 대신 `set_specific_charset()`를 사용하십시오.

**C++**

```cpp
#include <dicom.h>
#include <iostream>

using namespace dicom::literals;

dicom::DicomFile file;
auto& study = file.add_dataelement("StudyDescription"_tag, dicom::VR::LO);

// 이 바이트는 이미 UTF-8이지만, 데이터세트가 그 사실을 선언하지 않은 상태입니다.
study.from_string_view("심장 MRI");

if (!study.to_utf8_string()) {
    std::cout << "decode failed before declaration repair\n";
}

// 선언만 복구합니다. 기존 바이트는 그대로 유지됩니다.
file.set_declared_specific_charset(dicom::SpecificCharacterSet::ISO_IR_192);

if (auto utf8 = study.to_utf8_string()) {
    std::cout << *utf8 << '\n';
}
```

**파이썬**

```python
import dicomsdl as dicom

df = dicom.DicomFile()
study = df.dataset.add_dataelement(dicom.Tag("StudyDescription"), dicom.VR.LO)

# 이 바이트는 이미 UTF-8이지만, 데이터세트가 그 사실을 선언하지 않은 상태입니다.
study.from_string_view("심장 MRI")

print(study.to_utf8_string())

# 선언만 복구합니다. 기존 바이트는 그대로 유지됩니다.
df.set_declared_specific_charset("ISO_IR 192")

print(study.to_utf8_string())
```

## 다음으로 볼 곳

- 읽기/디코딩 실패: [오류 처리](error_handling.md)
- 문자 집합 텍스트 및 PN 개요: [문자 집합 및 사람 이름](charset_and_person_name.md)
- 중첩된 경로 문제: [시퀀스 및 경로](sequence_and_paths.md)
- 픽셀 인코딩 문제: [픽셀 인코딩 제약 조건](../reference/pixel_encode_constraints.md)
- 정확한 실패 카테고리: [오류 모델](../reference/error_model.md)
