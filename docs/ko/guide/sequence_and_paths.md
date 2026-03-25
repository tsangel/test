# 시퀀스 및 경로

중첩된 DICOM 데이터는 `SequenceKeyword.0.LeafKeyword`와 같은 점으로 구분된 태그 경로를 통해 읽고 쓰기가 가장 쉽습니다.

## C++

```cpp
#include <dicom.h>

dicom::DataSet ds;
ds.ensure_dataelement("ReferencedStudySequence.0.ReferencedSOPInstanceUID", dicom::VR::UI)
  .from_uid_string("1.2.3");

const auto& uid =
    ds["ReferencedStudySequence.0.ReferencedSOPInstanceUID"];
```

## 파이썬

```python
import dicomsdl as dicom

df = dicom.read_file("sample.dcm")
ds = df.dataset
half_life = ds.get_value("RadiopharmaceuticalInformationSequence.0.RadionuclideHalfLife")
ds.set_value("ReferencedStudySequence.0.ReferencedSOPInstanceUID", "1.2.3")
```

## 참고

- 경로 형식은 `SequenceName.item_index.LeafName`입니다.
- 중첩된 값을 한 번에 조회하거나 할당하고 싶다면 이 경로 형식을 사용하세요.
- C++에서는 `operator[]`, `get_dataelement(...)` 및 `get_value(...)`가 모두 점으로 구분된 태그 경로를 허용합니다.
- `ensure_dataelement(...)`가 기존 비시퀀스 중간 요소 아래에 중첩 경로를 실제로 만들 때는, 그 중간 요소를 `SQ`로 다시 설정할 수 있습니다.
- 저수준 순회 세부 정보가 필요하다면 반환된 `Sequence` / 아이템 데이터세트에 대한 참조를 유지하고 요소를 직접 확인하세요.

## 관련 문서

- [핵심 개체](core_objects.md)
- [C++ 데이터세트 가이드](cpp_dataset_guide.md)
- [Python 데이터세트 가이드](python_dataset_guide.md)
- [태그 경로 조회 의미](../reference/tag_path_lookup.md)
