# 핵심 객체

이 페이지는 DicomSDL에서 가장 자주 만나게 되는 객체와 보조 타입을 소개합니다: `DicomFile`, `DataSet`, `DataElement`, `Sequence`, `PixelSequence`, `Uid`, `PersonName`.

## DicomFile

`DicomFile`은 DicomSDL의 file/session wrapper입니다. root dataset을 소유하고, 상위 수준의 읽기, 쓰기, decode, transcode 동작을 함께 제공합니다. file/session 상태, decode, serialization이 중요할 때는 여기서 시작하세요.

관련 DICOM 표준 섹션:

- `DicomFile` 자체는 DICOM 표준에 이름이 있는 객체라기보다 DicomSDL의 구현 객체입니다.
- 가장 가까운 표준 대응은 [DICOM PS3.10 Chapter 7, DICOM File Format](https://dicom.nema.org/medical/dicom/current/output/chtml/part10/chapter_7.html), 특히 File Meta Information을 다루는 Section 7.1입니다.
- 내부의 root dataset은 [DICOM PS3.5 Chapter 7, The Data Set](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/chapter_7.html)을 따릅니다.

## DataSet

`DataSet`은 하나의 DICOM 객체를 이루는 전체 구조화된 field 집합입니다. DicomSDL의 `DataSet` 클래스는 이 DICOM Data Element 컨테이너를 구현합니다. 실제로는 metadata field를 읽거나 수정할 때 가장 자주 쓰는 객체이므로, 대부분의 metadata 접근과 갱신 흐름은 여기서 시작합니다.

Python에서는 일반적인 top-level metadata 읽기에 보통 `df.Rows` 또는 `ds.PatientName`을 가장 먼저 사용합니다. 키가 동적이거나 중첩되어 있으면 `get_value("Seq.0.Tag")`를 사용하고, 타입 값만이 아니라 `DataElement` 메타데이터가 필요하면 `ds["Rows"]`를 사용하세요.

관련 DICOM 표준 섹션:

- 주로 [DICOM PS3.5 Chapter 7, The Data Set](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/chapter_7.html)에 대응합니다.

## DataElement

`DataElement`는 `DataSet` 안의 하나의 field입니다. 예를 들면 `PatientName`이나 `Rows` 같은 항목입니다. DicomSDL의 `DataElement` 클래스는 이 단일 DICOM field와 함께 그 tag, VR, value, length, offset, 관련 metadata를 구현합니다. 값뿐 아니라 그 element의 구조 정보까지 같이 필요할 때는 여기서 시작하세요.

모든 `DataElement`에는 다음 두 가지 보조 개념이 붙습니다.

- `Tag`: field의 숫자 식별자입니다. 예를 들어 Patient Name은 `(0010,0010)`입니다.
- `VR`: `PN`, `SQ`, `US`, `OB` 같은 Value Representation입니다. 이 값은 field가 어떻게 인코딩되고 해석되는지를 알려줍니다.

관련 DICOM 표준 섹션:

- 주로 [DICOM PS3.5 Section 7.1, Data Elements](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/chapter_7.html)에 대응합니다.
- field와 layout 규칙은 Section 7.1.1부터 7.1.3에 설명되어 있습니다.
- `VR` 규칙은 주로 [DICOM PS3.5 Section 6.2, Value Representation](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/sect_6.2.html)에 대응합니다.

## Sequence

`Sequence`는 값이 nested item 목록으로 이루어진 DICOM field이며, 각 item은 다시 하나의 `DataSet`입니다. DicomSDL의 `Sequence` 클래스는 이 `SQ` 개념을 구현합니다. `Seq.0.Tag` 같은 nested DICOM 구조를 순회하거나 수정할 때는 여기서 시작하세요.

관련 DICOM 표준 섹션:

- 주로 [DICOM PS3.5 Section 7.5, Nesting of Data Sets](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/sect_7.5.html)에 대응합니다.
- item 인코딩 규칙은 Section 7.5.1에 설명되어 있습니다.

## PixelSequence

`PixelSequence`는 `PixelData`가 encapsulated 또는 compressed 형태로 저장될 때 사용되는 컨테이너입니다. `Sequence`와 달리 일반적인 nested DICOM field는 아닙니다. 이는 compressed pixel payload 뒤에 있는 frame/fragment 컨테이너입니다. DicomSDL의 `PixelSequence` 클래스는 이 특수한 pixel storage 객체를 구현합니다. Python에서는 보통 `elem.pixel_sequence`, C++에서는 `as_pixel_sequence()`로 도달하며, compressed `PixelData`에서 encoded frame bytes나 fragment 수준 접근이 필요할 때 여기서 시작합니다.

관련 DICOM 표준 섹션:

- encapsulated pixel storage는 [DICOM PS3.5 Section 8.2, Native or Encapsulated Format Encoding](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/sect_8.2.html)과 [Section A.4, Transfer Syntaxes for Encapsulation of Encoded Pixel Data](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/sect_A.4.html)에 정의되어 있습니다.
- 주변 pixel attribute는 여전히 [DICOM PS3.3 Section C.7.6.3, Image Pixel Module](https://dicom.nema.org/medical/dicom/current/output/chtml/part03/sect_C.7.6.3.html)을 따릅니다.

## Supporting Types

`Uid`와 `PersonName`은 상위 컨테이너보다는 보조 value type이지만, 실제 코드에서는 계속 등장합니다.

- `Uid`: SOP Class UID, SOP Instance UID, Transfer Syntax UID 같은 DICOM unique identifier 문자열을 감싼 wrapper
- `PersonName`: `PN` 값을 구조화한 표현. Python에서는 `PN` field의 `.value`가 일반 문자열이 아니라 `PersonName(...)` 객체로 보일 수 있습니다.

관련 DICOM 표준 섹션:

- `PersonName` 규칙은 주로 [DICOM PS3.5 Section 6.2, Value Representation](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/sect_6.2.html), 특히 `PN`을 다루는 Section 6.2.1에 대응합니다.
- `Uid` 규칙은 주로 [DICOM PS3.5 Chapter 9, Unique Identifiers (UIDs)](https://dicom.nema.org/medical/dicom/current/output/chtml/part05/chapter_9.html)에 대응합니다.

## 관련 문서

- [Sequence and Paths](sequence_and_paths.md)
- [DataElement Reference](../reference/dataelement_reference.md)
- [Sequence Reference](../reference/sequence_reference.md)
- [Pixel Decode](pixel_decode.md)
- [Pixel Reference](../reference/pixel_reference.md)
- [Charset and Person Name](charset_and_person_name.md)
- [Charset Reference](../reference/charset_reference.md)
- [C++ DataSet Guide](cpp_dataset_guide.md)
- [Python DataSet Guide](python_dataset_guide.md)
