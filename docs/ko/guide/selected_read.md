# Selected Read

`read_file_selected(...)`와 `read_bytes_selected(...)`는 DICOM 스트림에서
필요한 태그와 sequence 하위 항목만 골라 읽습니다.

top-level 태그 일부와 nested sequence 안의 특정 child만 필요하고,
나머지 dataset 전체를 읽어 들이고 싶지 않을 때 selected read를 사용하세요.

## 선택 트리

`DataSetSelection`은 중첩 구조를 표현하는 선택 트리입니다.

- 자식이 없는 leaf node는 해당 tag 자체만 선택합니다.
- 자식이 있는 node는 해당 tag를 선택하고, 그 sequence 아래의 모든 item dataset에
  child selection을 적용합니다.
- private tag와 unknown tag도 사용할 수 있으며, `"70531000"` 같은 명시적 tag string도 허용됩니다.

생성할 때는 다음 정규화가 자동으로 적용됩니다.

- 루트 레벨에 `TransferSyntaxUID (0002,0010)`와
  `SpecificCharacterSet (0008,0005)`가 없으면 자동으로 포함
- sibling tag를 오름차순으로 정렬
- 중복 node를 병합

## C++ 예시

```cpp
#include <dicom.h>
using namespace dicom::literals;

dicom::DataSetSelection selection{
    "StudyInstanceUID"_tag,
    "SeriesInstanceUID"_tag,
    {"ReferencedSeriesSequence"_tag, {
        "SeriesInstanceUID"_tag,
        "ReferencedSOPInstanceUID"_tag,
    }},
    "SOPInstanceUID"_tag,
};

auto file = dicom::read_file_selected("sample.dcm", selection);
auto& ds = file->dataset();
```

## Python에서 재사용하는 예

```python
import dicomsdl as dicom

selection = dicom.DataSetSelection(
    [
        "StudyInstanceUID",
        "SeriesInstanceUID",
        ("ReferencedSeriesSequence", [
            "SeriesInstanceUID",
            "ReferencedSOPInstanceUID",
        ]),
        "SOPInstanceUID",
    ]
)

df = dicom.read_file_selected("sample.dcm", selection)
```

## Python one-shot 예시

Python에서는 one-shot 호출에 raw nested selection tree를 바로 넘길 수도 있습니다.

```python
import dicomsdl as dicom

df = dicom.read_bytes_selected(
    data,
    [
        "SOPInstanceUID",
        ("ReferencedStudySequence", [
            "ReferencedSOPInstanceUID",
        ]),
    ],
    name="sample",
)
```

같은 selection을 여러 파일에 반복해서 적용할 계획이라면,
`DataSetSelection(...)`을 한 번 만들어 재사용하는 편이 좋습니다.

## 동작 규칙

- 반환되는 `DicomFile`에는 선택한 태그와 sequence 하위 항목만 들어 있습니다.
  선택하지 않은 태그는 없는 것처럼 동작합니다.
- 루트 레벨의 `TransferSyntaxUID`와 `SpecificCharacterSet`는
  selection에 직접 적지 않아도 항상 고려됩니다.
- selected-read API에서는 `ReadOptions.load_until`을 무시합니다.
  읽기 경계는 selection을 기준으로 내부에서 계산합니다.
- `SQ`만 선택했더라도 source에 그 sequence가 있으면 해당 sequence element는 유지됩니다.
  item dataset도 유지되지만, 선택된 child가 없으면 item dataset은 비어 있을 수 있습니다.
- private tag와 unknown tag도 selection 대상으로 사용할 수 있으며,
  `"70531000"` 같은 명시적 tag string도 쓸 수 있습니다.
- `keep_on_error`는 일반 read와 비슷하게 동작하지만,
  selected read가 실제로 방문한 영역에 대해서만 적용됩니다.
- 선택된 영역 밖에 있는 malformed data는 아예 보이지 않을 수 있으므로,
  그런 손상은 `has_error`나 `error_message`에 반영되지 않을 수 있습니다.

## 관련 문서

- [File IO](file_io.md)
- [DataSet Walk](dataset_walk.md)
- [C++ API Overview](../reference/cpp_api.md)
- [DicomFile Reference](../reference/dicomfile_reference.md)
- [Python API Reference](../reference/python_reference.md)
