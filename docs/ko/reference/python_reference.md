# Python API Reference

```{note}
이 페이지 본문은 아직 영어 원문입니다. 필요하면 영문 페이지를 기준으로 읽어 주세요.
```

The user-facing Python guide lives in
[Python DataSet Guide](../guide/python_dataset_guide.md).

Use that page for:

- the recommended Python access patterns
- `DataSet` / `DicomFile` / `DataElement` behavior
- value reads and writes
- presence checks
- explicit VR assignment for private tags

DICOM JSON Model 읽기/쓰기에 대해서는
[DICOM JSON](../guide/dicom_json.md)을 참고하세요.

## DICOM JSON

- `read_json(source, name="<memory>", charset_errors="strict")`는 UTF-8
  DICOM JSON 텍스트에서 top-level 데이터세트 객체 하나 또는 데이터세트 객체
  배열 하나를 읽습니다.
- Python에서 `DicomFile.write_json(...)`와 `DataSet.write_json(...)`는
  `(json_text, bulk_parts)`를 반환합니다.
- `JsonBulkRef`는 아직 다운로드해서 `set_bulk_data(...)`로
  `DicomFile`에 채워 넣어야 하는 `BulkDataURI` payload를 설명합니다.
- `read_json(...)`는 opaque presigned URL이나 토큰이 붙은 다운로드 URL은
  그대로 보존하고, URI 모양이 frame 확장을 명확히 지원할 때만 frame URL을
  합성합니다.

## Selected read

- `DataSetSelection([...])`는 canonicalized nested selection tree를 만듭니다.
- `read_file_selected(path, selection, keep_on_error=None)`는 디스크에서 선택한 태그와 sequence 하위 항목만 담긴 `DicomFile`을 읽습니다.
- `read_bytes_selected(data, selection, name="<memory>", keep_on_error=None, copy=True)`는
  bytes-like object에서 같은 방식으로 선택한 부분만 읽습니다.
- `selection`에는 재사용 가능한 `DataSetSelection`을 넘겨도 되고, one-shot 호출을
  위해 leaf tag와 `(tag, children)` pair로 이루어진 raw nested Python sequence를
  바로 넘겨도 됩니다.
- `TransferSyntaxUID`와 `SpecificCharacterSet`는 selection에 없어도 root level에서
  항상 고려됩니다.
- `ReadOptions.load_until`은 selected-read API에 적용되지 않습니다.
- private tag와 unknown tag도 selection 대상으로 사용할 수 있으며, `"70531000"` 같은 explicit tag string도 사용할 수 있습니다.
- `SQ`만 선택해도 present한 sequence와 item count는 유지되지만, child item dataset은
  비어 있을 수 있습니다.
- 선택된 영역 밖의 malformed data는 보이지 않을 수 있으므로, `has_error`와
  `error_message`는 selected read가 실제로 방문한 영역만 설명합니다.

## Supporting types

### Tag

Construct from:

- `(group, element)`
- packed `int`
- keyword or Tag string

Important properties:

- `group`
- `element`
- `value`
- `is_private`

`str(tag)` renders as `(gggg,eeee)`.

### VR

Enum-like VR wrapper with constants such as `VR.AE`, `VR.US`, and `VR.UI`.

Useful helpers:

- `str(vr)` / `vr.str()`
- `is_string()`
- `is_binary()`
- `is_sequence()`
- `is_pixel_sequence()`
- `uses_specific_character_set()`
- `allows_multiple_text_values()`

### Uid

Construct from a keyword or dotted string.
Unknown values raise.

## Related docs

- [Python DataSet Guide](../guide/python_dataset_guide.md)
- [Selected Read](../guide/selected_read.md)
- [Charset and Person Name](../guide/charset_and_person_name.md)
