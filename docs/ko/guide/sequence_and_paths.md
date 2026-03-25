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

## 메모

- 경로 형식은 `SequenceName.item_index.LeafName`입니다.
- 일회성 중첩 조회 또는 할당을 원하는 경우 이 경로 양식을 사용하세요.
- C++에서는 `operator[]`, `get_dataelement(...)` 및 `get_value(...)`가 모두 점으로 구분된 태그 경로를 허용합니다.
- `ensure_dataelement(...)`가 기존 비순차 중간 요소 아래에 중첩된 경로를 구체화하면 해당 중간 요소를 `SQ`로 재설정할 수 있습니다.
- 낮은 수준의 순회 세부정보가 필요한 경우 반환된 `Sequence` / 항목 데이터세트에 대한 참조를 유지하고 요소를 직접 검사하세요.

## 관련 문서

- [핵심 개체](core_objects.md)
- [C++ 데이터세트 가이드](cpp_dataset_guide.md)
- [Python 데이터세트 가이드](python_dataset_guide.md)
- [태그 경로 조회 의미](../reference/tag_path_lookup.md)
