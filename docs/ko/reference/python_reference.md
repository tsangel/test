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

- `DataSetSelection([...])`??canonicalized nested selection tree瑜?留뚮벊?덈떎.
- `read_file_selected(path, selection, keep_on_error=None)`???붿뒪?ъ뿉???좏깮???쒓렇? sequence ?섏쐞 ??ぉ留??닿릿 `DicomFile`???쎌뒿?덈떎.
- `read_bytes_selected(data, selection, name="<memory>", keep_on_error=None, copy=True)`??  bytes-like object?먯꽌 媛숈? 諛⑹떇?쇰줈 ?좏깮??遺遺꾨쭔 ?쎌뒿?덈떎.
- `selection`?먮뒗 ?ъ궗??媛?ν븳 `DataSetSelection`???섍꺼???섍퀬, one-shot ?몄텧??  ?꾪빐 leaf tag? `(tag, children)` pair濡??대（?댁쭊 raw nested Python sequence瑜?  諛붾줈 ?섍꺼???⑸땲??
- `TransferSyntaxUID`? `SpecificCharacterSet`??selection???놁뼱??root level?먯꽌
  ??긽 怨좊젮?⑸땲??
- `ReadOptions.load_until`? selected-read API???곸슜?섏? ?딆뒿?덈떎.
- private tag? unknown tag??selection ??곸쑝濡??ъ슜?????덉쑝硫? `"70531000"` 媛숈? explicit tag string???ъ슜?????덉뒿?덈떎.
- `SQ`留??좏깮?대룄 present??sequence? item count???좎??섏?留? child item dataset?
  鍮꾩뼱 ?덉쓣 ???덉뒿?덈떎.
- ?좏깮???곸뿭 諛뽰쓽 malformed data??蹂댁씠吏 ?딆쓣 ???덉쑝誘濡? `has_error`?
  `error_message`??selected read媛 ?ㅼ젣濡?諛⑸Ц???곸뿭留??ㅻ챸?⑸땲??

## Pixel decode metadata

- `DecodeInfo` is the Python-side decode metadata object returned by
  `to_array(..., with_info=True)` and `decode_into(..., with_info=True)`.
- `DecodeInfo.photometric` is an optional `Photometric` value and may be `None`
  when the successful decode result does not map cleanly to a DICOM photometric.
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
