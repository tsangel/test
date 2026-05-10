# Python API Reference

```{note}
???섏씠吏 蹂몃Ц? ?꾩쭅 ?곸뼱 ?먮Ц?낅땲?? ?꾩슂?섎㈃ ?곷Ц ?섏씠吏瑜?湲곗??쇰줈 ?쎌뼱 二쇱꽭??
```

The user-facing Python guide lives in
[Python DataSet Guide](../guide/python_dataset_guide.md).

Use that page for:

- the recommended Python access patterns
- `DataSet` / `DicomFile` / `DataElement` behavior
- value reads and writes
- presence checks
- explicit VR assignment for private tags

DICOM JSON Model ?쎄린/?곌린????댁꽌??[DICOM JSON](../guide/dicom_json.md)??李멸퀬?섏꽭??

## DICOM JSON

- `read_json(source, name="<memory>", charset_errors="strict")`??硫붾え由ъ뿉
  ?대? ?덈뒗 UTF-8 DICOM JSON ?띿뒪???먮뒗 諛붿씠?몄뿉??top-level ?곗씠?곗꽭??  媛앹껜 ?섎굹 ?먮뒗 ?곗씠?곗꽭??媛앹껜 諛곗뿴 ?섎굹瑜??쎌뒿?덈떎.
- Python?먯꽌 `DicomFile.write_json(...)`? `DataSet.write_json(...)`??  `(json_text, bulk_parts)`瑜?諛섑솚?⑸땲??
- `JsonBulkRef`???꾩쭅 ?ㅼ슫濡쒕뱶?댁꽌 `set_bulk_data(...)`濡?  `DicomFile`??梨꾩썙 ?ｌ뼱???섎뒗 `BulkDataURI` payload瑜??ㅻ챸?⑸땲??
- `read_json(...)`??opaque presigned URL?대굹 ?좏겙??遺숈? ?ㅼ슫濡쒕뱶 URL?
  洹몃?濡?蹂댁〈?섍퀬, URI 紐⑥뼇??frame ?뺤옣??紐낇솗??吏?먰븷 ?뚮쭔 frame URL??  ?⑹꽦?⑸땲??

## Selected read

- `DataSetSelection([...])`은 canonicalized nested selection tree를 만듭니다.
- `read_file_selected(path, selection, keep_on_error=None, *, load_until=None)`는
  디스크에서 선택한 tag와 nested sequence child만 유지하는 `DicomFile`을 읽습니다.
- `read_bytes_selected(data, selection, name="<memory>", keep_on_error=None, copy=True, *, load_until=None)`는
  bytes-like object에서 같은 selected 결과를 읽습니다.
- `continue_read_selected(file, selection, *, load_until=None, keep_on_error=None)`는
  일부만 읽힌 `DicomFile`의 현재 stream 위치부터 selected read를 in-place로 이어갑니다.
  기존 element는 유지됩니다.
- `selection`에는 재사용 가능한 `DataSetSelection`을 넘길 수도 있고, one-shot 호출에서는
  leaf tag와 `(tag, children)` pair로 이루어진 raw nested Python sequence를 바로 넘길 수도 있습니다.
- `TransferSyntaxUID`와 `SpecificCharacterSet`은 selection에 없어도 root level에서 항상 고려됩니다.
- `load_until`은 selected-read frontier의 상한입니다. 실제 root stop tag는
  `min(마지막 selected root tag, load_until)`입니다.
- private tag와 unknown tag도 selection 대상으로 사용할 수 있으며,
  `"70531000"` 같은 explicit tag string도 사용할 수 있습니다.
- `SQ`만 선택해도 present sequence와 item count는 유지되지만 child item dataset은
  비어 있을 수 있습니다.
- 선택 영역 밖의 malformed data는 보이지 않을 수 있으므로 `has_error`와
  `error_message`는 selected read가 실제로 방문한 영역만 설명합니다.

## Pixel decode metadata

- `DecodeInfo` is the Python-side decode metadata object returned by
  `to_array(..., with_info=True)` and `decode_into(..., with_info=True)`.
- `DecodeInfo.photometric` is an optional `Photometric` value and may be `None`
  when the successful decode result does not map cleanly to a DICOM photometric.
  It reflects the backend's actual decoded output when representable, so it may
  differ from stored metadata such as `YBR_RCT` / `YBR_ICT`.
  Some backends may ignore `decode_mct` and still return RGB-domain samples; in
  that case `DecodeInfo.photometric` reports `Photometric.rgb`.
- `DecodeInfo.encoded_lossy_state` is an `EncodedLossyState` enum describing whether
  the encoded source is `lossless`, `lossy`, `near_lossless`, or `unknown`.
- `DecodeInfo.dtype`, `DecodeInfo.planar`, and `DecodeInfo.bits_per_sample`
  describe the decoded output buffer that was produced successfully.
- For `frame=-1` on multi-frame input, Python `with_info=True` reports
  frame-0/common decode metadata while still returning or filling the full
  decoded volume.

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
- [Pixel Decode](../guide/pixel_decode.md)
- [Selected Read](../guide/selected_read.md)
- [Charset and Person Name](../guide/charset_and_person_name.md)
