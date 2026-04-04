# DICOM JSON

DicomSDL은 `DataSet` 또는 `DicomFile`을 DICOM JSON Model로 직렬화할 수 있고,
DICOM JSON을 다시 하나 이상의 `DicomFile` 객체로 읽어들일 수 있습니다.

이 페이지는 Python과 C++에서 공용으로 사용하는 `write_json(...)`,
`read_json(...)`, `set_bulk_data(...)` 흐름에 집중합니다.

## 지원 범위

- `DicomFile.write_json(...)`
- `DataSet.write_json(...)`
- 메모리에 이미 있는 UTF-8 텍스트 또는 바이트에서 `read_json(...)`
- DICOM JSON top-level object 및 top-level array payload
- `BulkDataURI`, `InlineBinary`, 중첩 sequence, PN object
- `JsonBulkRef` + `set_bulk_data(...)`를 통한 호출자 주도 bulk 다운로드

현재 범위 관련 참고:

- `read_json(...)`은 메모리 입력 API입니다. 디스크나 HTTP 스트림을 직접 읽지는 않습니다.
- JSON reader/writer는 DICOM JSON Model을 구현하며, 완전한 DICOMweb HTTP
  클라이언트/서버 스택은 아닙니다.

## JSON 쓰기

### Python 쓰기 예제

```python
import dicomsdl as dicom

df = dicom.read_file("sample.dcm")

json_text, bulk_parts = df.write_json()
```

반환값은 다음과 같습니다.

- `json_text: str`
- `bulk_parts: list[tuple[str, memoryview, str, str]]`

각 bulk tuple은 다음 값을 가집니다.

- `uri`
- `payload`
- `media_type`
- `transfer_syntax_uid`

### C++ 쓰기 예제

```cpp
#include <dicom.h>

auto file = dicom::read_file("sample.dcm");
dicom::JsonWriteResult out = file->write_json();

std::string json_text = std::move(out.json);
for (const auto& part : out.bulk_parts) {
    auto bytes = part.bytes();
    // part.uri
    // part.media_type
    // part.transfer_syntax_uid
}
```

## JSON 쓰기 옵션

`JsonWriteOptions`는 공개 헤더에 정의되어 있고 Python에서는 keyword
argument로 노출됩니다.

### `include_group_0002`

- 기본값: `false`
- 의미: JSON 출력에 file meta group `0002`를 포함할지 여부

기본적으로 DICOM JSON / DICOMweb 스타일 출력은 group `0002`를 제외합니다.
Group length element `(gggg,0000)`는 항상 제외됩니다.

### `bulk_data`

Python 값:

- `"inline"`
- `"uri"`
- `"omit"`

C++ 값:

- `JsonBulkDataMode::inline_`
- `JsonBulkDataMode::uri`
- `JsonBulkDataMode::omit`

동작:

- `inline`: bulk 가능한 값도 `InlineBinary`로 유지
- `uri`: threshold 이상 값은 `BulkDataURI`로 분리
- `omit`: attribute 자체는 `vr`과 함께 남기고 bulk 값만 출력하지 않음

### `bulk_data_threshold`

- 기본값: `1024`
- `bulk_data="uri"`일 때만 사용

`bulk_data="uri"`인 경우 threshold보다 작은 값은 inline으로 남고,
threshold 이상 값은 `BulkDataURI`가 됩니다.

### `bulk_data_uri_template`

`bulk_data="uri"`일 때 `PixelData`가 아닌 bulk element에 사용하는
URI template입니다.

지원 placeholder:

- `{study}`
- `{series}`
- `{instance}`
- `{tag}`

`{tag}` 확장 규칙:

- top-level element: `7FE00010`
- 중첩 sequence element: `22002200.0.12340012` 같은 dotted tag path

예시:

```python
json_text, bulk_parts = df.write_json(
    bulk_data="uri",
    bulk_data_uri_template="/dicomweb/studies/{study}/series/{series}/instances/{instance}/bulk/{tag}",
)
```

### `pixel_data_uri_template`

`PixelData (7FE0,0010)` 전용 override입니다.

전형적인 사용 예:

```python
json_text, bulk_parts = df.write_json(
    bulk_data="uri",
    bulk_data_uri_template="/dicomweb/studies/{study}/series/{series}/instances/{instance}/bulk/{tag}",
    pixel_data_uri_template="/dicomweb/studies/{study}/series/{series}/instances/{instance}/frames",
)
```

서버에서 frame 단위 pixel route를 다른 bulk data route와 분리해 둘 때
사용합니다.

### 쓰기 `charset_errors`

Python 값:

- `"strict"`
- `"replace_fffd"`
- `"replace_hex_escape"`

C++ 값:

- `CharsetDecodeErrorPolicy::strict`
- `CharsetDecodeErrorPolicy::replace_fffd`
- `CharsetDecodeErrorPolicy::replace_hex_escape`

JSON 텍스트를 생성할 때 텍스트 decode 처리 방식을 제어합니다.

## PixelData bulk 동작

### Native PixelData

- JSON에는 `BulkDataURI` 하나만 유지됩니다.
- native multi-frame bulk도 aggregate bulk part 하나로 유지됩니다.

### Encapsulated PixelData

- JSON에는 여전히 base `BulkDataURI` 하나만 유지됩니다.
- `bulk_parts`는 frame 단위로 반환됩니다.
- frame URI는 선택한 base에 따라 다음처럼 정해집니다.
  - `/.../frames` -> `/.../frames/1`, `/.../frames/2`, ...
  - generic base URI -> `/.../bulk/7FE00010/frames/1`, ...

이 방식은 JSON을 간결하게 유지하면서도 multipart 응답이나 frame 응답을
조립할 때 필요한 per-frame payload 목록을 제공합니다.

## JSON 읽기

### Python 읽기 예제

```python
import dicomsdl as dicom

items = dicom.read_json(json_text)

for df, refs in items:
    ...
```

### C++ 읽기 예제

```cpp
#include <dicom.h>

dicom::JsonReadResult result = dicom::read_json(
    reinterpret_cast<const std::uint8_t*>(json_bytes.data()),
    json_bytes.size());

for (auto& item : result.items) {
    auto& file = *item.file;
    auto& refs = item.pending_bulk_data;
    (void)file;
    (void)refs;
}
```

reader는 DICOM JSON이 다음 둘 중 하나일 수 있기 때문에 항상 collection을
반환합니다.

- 데이터세트 객체 하나
- 데이터세트 객체 배열

JSON이 top-level object 하나라면 결과 list 길이는 `1`입니다.

## JSON 읽기 옵션

### 읽기 `charset_errors`

Python 값:

- `"strict"`
- `"replace_qmark"`
- `"replace_unicode_escape"`

C++ 값:

- `CharsetEncodeErrorPolicy::strict`
- `CharsetEncodeErrorPolicy::replace_qmark`
- `CharsetEncodeErrorPolicy::replace_unicode_escape`

이 정책은 UTF-8 JSON에서 읽어들인 텍스트를 나중에 `value_span()`,
`write_file(...)`, `set_bulk_data(...)` 같은 API를 위해 raw DICOM bytes로
변환할 때 사용됩니다.

## bulk 다운로드 흐름

전형적인 Python 흐름:

```python
items = dicom.read_json(json_text)

for df, refs in items:
    for ref in refs:
        payload = download(ref.uri)
        df.set_bulk_data(ref, payload)
```

전형적인 C++ 흐름:

```cpp
for (auto& item : result.items) {
    for (const auto& ref : item.pending_bulk_data) {
        std::vector<std::uint8_t> payload = download(ref.uri);
        item.file->set_bulk_data(ref, payload);
    }
}
```

`JsonBulkRef`에는 다음 정보가 들어 있습니다.

- `kind`
- `path`
- `frame_index`
- `uri`
- `media_type`
- `transfer_syntax_uid`
- `vr`

## 읽기 시 URI 보존 규칙

JSON reader는 의도적으로 보수적으로 동작합니다.

다음처럼 이미 역참조 가능한 URI는 그대로 보존합니다.

- `.../frames/1`
- `.../frames/1,2,3`
- `https://example.test/instances/1?sig=...` 같은 presigned URL, 토큰이 붙은
  다운로드 URL, 또는 opaque absolute URL
- `https://example.test/studies/s/series/r/instances/i/bulk/7FE00010?sig=...`
  같은 presigned 또는 토큰 기반 generic pixel URL

다음처럼 URI 모양 자체가 frame route를 명확히 드러낼 때만 frame URL을
합성합니다.

- `.../frames`
- `.../bulk/7FE00010` 같은 서명/토큰 suffix가 없는 plain generic base URI

이 점은 presigned URL이나 토큰이 붙은 다운로드 URL에서 중요합니다. 이미
서명된 opaque URL에 `/frames/{n}`을 덧붙이면 path가 바뀌어 보통 역참조가
깨지므로, 이런 URI는 변형하지 않고 그대로 둡니다.

## `set_bulk_data(...)` 동작

`set_bulk_data(...)`는 다음 두 가지 중요한 경우를 지원합니다.

- frame ref: 인코딩된 frame 하나를 encapsulated `PixelData` slot에 복사
- opaque encapsulated element ref: encapsulated `PixelData` value field 전체를
  받아 쓸 수 있는 내부 픽셀 시퀀스로 복원

즉 opaque presigned `BulkDataURI`나 토큰이 붙은 `BulkDataURI`도 일반적인
흐름에 참여할 수 있습니다.

1. `read_json(...)`가 presigned 또는 토큰이 붙은 다운로드 URL을
   `element` ref 하나로 그대로 보존
2. 호출자가 그 URL에서 payload bytes를 다운로드
3. `set_bulk_data(ref, payload)`가 다운로드한 value field로부터
   encapsulated `PixelData`를 실제 내부 픽셀 시퀀스로 복원

## 전송 구문 관련 참고

`JsonBulkPart.transfer_syntax_uid`와 `JsonBulkRef.transfer_syntax_uid`는 file
meta의 `TransferSyntaxUID (0002,0010)`가 있을 때 그 값으로 채워집니다.
그 정보가 없으면 reader는 보수적으로 동작하며, metadata만 보고 encapsulated
frame layout을 추측하지 않습니다.

## 입력 규칙

- JSON 입력은 UTF-8 텍스트여야 합니다.
- Python은 `str` 또는 bytes-like 입력을 받을 수 있습니다.
- 빈 입력은 에러입니다.
- top-level 입력은 JSON object 또는 array여야 합니다.

## 관련 문서

- [파일 I/O](file_io.md)
- [Python 데이터세트 가이드](python_dataset_guide.md)
- [Python API Reference](../reference/python_reference.md)
- [DicomFile Reference](../reference/dicomfile_reference.md)
- [DataSet Reference](../reference/dataset_reference.md)
