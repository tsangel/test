import dicomsdl as dicom
import pathlib
import pytest


def test_read_json_native_multiframe_keeps_one_bulk_ref():
    json_text = (
        '{"00080016":{"Value":["1.2.840.10008.5.1.4.1.1.2"]},'
        '"00100010":{"vr":"PN","Value":[{"Alphabetic":"Doe^Jane"}]},'
        '"00280008":{"vr":"IS","Value":[2]},'
        '"7FE00010":{"vr":"OW","BulkDataURI":"instances/1/frames"}}'
    )

    items = dicom.read_json(json_text)
    assert len(items) == 1

    df, refs = items[0]
    assert isinstance(df, dicom.DicomFile)
    assert df.get_dataelement("SOPClassUID").to_string_view() == "1.2.840.10008.5.1.4.1.1.2"
    assert df.get_dataelement("PatientName").to_utf8_string() == "Doe^Jane"
    assert len(refs) == 1
    assert refs[0].kind == dicom.JsonBulkTargetKind.element
    assert refs[0].path == "7FE00010"
    assert refs[0].uri == "instances/1/frames"
    assert refs[0].media_type == "application/octet-stream"
    assert refs[0].transfer_syntax_uid == ""


def test_read_json_encapsulated_multiframe_expands_frame_refs():
    json_text = (
        '{"00020010":{"vr":"UI","Value":["1.2.840.10008.1.2.4.50"]},'
        '"00280008":{"vr":"IS","Value":[2]},'
        '"7FE00010":{"vr":"OB","BulkDataURI":"instances/1/frames"}}'
    )

    items = dicom.read_json(json_text.encode("utf-8"))
    assert len(items) == 1

    _df, refs = items[0]
    assert len(refs) == 2
    assert refs[0].kind == dicom.JsonBulkTargetKind.pixel_frame
    assert refs[0].frame_index == 0
    assert refs[0].uri == "instances/1/frames/1"
    assert refs[0].media_type == "image/jpeg"
    assert refs[0].transfer_syntax_uid == "1.2.840.10008.1.2.4.50"
    assert refs[1].kind == dicom.JsonBulkTargetKind.pixel_frame
    assert refs[1].frame_index == 1
    assert refs[1].uri == "instances/1/frames/2"
    assert refs[1].media_type == "image/jpeg"
    assert refs[1].transfer_syntax_uid == "1.2.840.10008.1.2.4.50"


def test_read_json_available_transfer_syntax_uid_does_not_expand_frame_refs():
    json_text = (
        '{"00083002":{"vr":"UI","Value":["1.2.840.10008.1.2.4.50"]},'
        '"00280008":{"vr":"IS","Value":[2]},'
        '"7FE00010":{"vr":"OB","BulkDataURI":"instances/1/frames"}}'
    )

    items = dicom.read_json(json_text)
    _df, refs = items[0]
    assert len(refs) == 1
    assert refs[0].kind == dicom.JsonBulkTargetKind.element
    assert refs[0].uri == "instances/1/frames"
    assert refs[0].media_type == "application/octet-stream"
    assert refs[0].transfer_syntax_uid == ""


def test_read_json_encapsulated_generic_bulk_uri_expands_frame_refs():
    json_text = (
        '{"00020010":{"vr":"UI","Value":["1.2.840.10008.1.2.4.50"]},'
        '"00280008":{"vr":"IS","Value":[1]},'
        '"7FE00010":{"vr":"OB","BulkDataURI":"instances/1/bulk/7FE00010"}}'
    )

    items = dicom.read_json(json_text)
    _df, refs = items[0]
    assert len(refs) == 1
    assert refs[0].kind == dicom.JsonBulkTargetKind.pixel_frame
    assert refs[0].frame_index == 0
    assert refs[0].uri == "instances/1/bulk/7FE00010/frames/1"
    assert refs[0].media_type == "image/jpeg"
    assert refs[0].transfer_syntax_uid == "1.2.840.10008.1.2.4.50"


def test_read_json_preserves_existing_frame_specific_bulk_uri():
    json_text = (
        '{"00020010":{"vr":"UI","Value":["1.2.840.10008.1.2.4.50"]},'
        '"00280008":{"vr":"IS","Value":[1]},'
        '"7FE00010":{"vr":"OB","BulkDataURI":"instances/1/frames/1"}}'
    )

    items = dicom.read_json(json_text)
    _df, refs = items[0]
    assert len(refs) == 1
    assert refs[0].kind == dicom.JsonBulkTargetKind.pixel_frame
    assert refs[0].frame_index == 0
    assert refs[0].uri == "instances/1/frames/1"
    assert refs[0].media_type == "image/jpeg"
    assert refs[0].transfer_syntax_uid == "1.2.840.10008.1.2.4.50"


def test_read_json_preserves_existing_frame_list_bulk_uri():
    json_text = (
        '{"00020010":{"vr":"UI","Value":["1.2.840.10008.1.2.4.50"]},'
        '"00280008":{"vr":"IS","Value":[3]},'
        '"7FE00010":{"vr":"OB","BulkDataURI":"instances/1/frames/1,2,3"}}'
    )

    items = dicom.read_json(json_text)
    _df, refs = items[0]
    assert len(refs) == 3
    assert refs[0].kind == dicom.JsonBulkTargetKind.pixel_frame
    assert refs[0].frame_index == 0
    assert refs[0].uri == "instances/1/frames/1"
    assert refs[1].frame_index == 1
    assert refs[1].uri == "instances/1/frames/2"
    assert refs[2].frame_index == 2
    assert refs[2].uri == "instances/1/frames/3"


def test_read_json_preserves_existing_frame_specific_bulk_uri_with_query_string():
    json_text = (
        '{"00020010":{"vr":"UI","Value":["1.2.840.10008.1.2.4.50"]},'
        '"00280008":{"vr":"IS","Value":[1]},'
        '"7FE00010":{"vr":"OB","BulkDataURI":"https://example.test/instances/1/frames/1?sig=abc"}}'
    )

    items = dicom.read_json(json_text)
    _df, refs = items[0]
    assert len(refs) == 1
    assert refs[0].kind == dicom.JsonBulkTargetKind.pixel_frame
    assert refs[0].frame_index == 0
    assert refs[0].uri == "https://example.test/instances/1/frames/1?sig=abc"


def test_read_json_preserves_existing_frame_list_bulk_uri_with_query_string():
    json_text = (
        '{"00020010":{"vr":"UI","Value":["1.2.840.10008.1.2.4.50"]},'
        '"00280008":{"vr":"IS","Value":[3]},'
        '"7FE00010":{"vr":"OB","BulkDataURI":"https://example.test/instances/1/frames/1,2,3?sig=abc"}}'
    )

    items = dicom.read_json(json_text)
    _df, refs = items[0]
    assert len(refs) == 3
    assert refs[0].kind == dicom.JsonBulkTargetKind.pixel_frame
    assert refs[0].frame_index == 0
    assert refs[0].uri == "https://example.test/instances/1/frames/1?sig=abc"
    assert refs[1].frame_index == 1
    assert refs[1].uri == "https://example.test/instances/1/frames/2?sig=abc"
    assert refs[2].frame_index == 2
    assert refs[2].uri == "https://example.test/instances/1/frames/3?sig=abc"


def test_read_json_keeps_opaque_signed_pixel_bulk_uri_as_element_ref():
    json_text = (
        '{"00020010":{"vr":"UI","Value":["1.2.840.10008.1.2.4.50"]},'
        '"00280008":{"vr":"IS","Value":[3]},'
        '"7FE00010":{"vr":"OB","BulkDataURI":"https://example.test/instances/1?sig=abc"}}'
    )

    items = dicom.read_json(json_text)
    _df, refs = items[0]
    assert len(refs) == 1
    assert refs[0].kind == dicom.JsonBulkTargetKind.element
    assert refs[0].uri == "https://example.test/instances/1?sig=abc"
    assert refs[0].media_type == "image/jpeg"
    assert refs[0].transfer_syntax_uid == "1.2.840.10008.1.2.4.50"


def test_read_json_set_bulk_data_populates_targets():
    native_json = '{"7FE00010":{"vr":"OW","BulkDataURI":"instances/1/frames"}}'
    native_items = dicom.read_json(native_json)
    native_df, native_refs = native_items[0]
    assert native_df.set_bulk_data(native_refs[0], b"\x01\x00\x02\x00\x03\x00\x04\x00")
    assert bytes(native_df["PixelData"].value_span()) == b"\x01\x00\x02\x00\x03\x00\x04\x00"

    encap_json = (
        '{"00020010":{"vr":"UI","Value":["1.2.840.10008.1.2.4.50"]},'
        '"00280008":{"vr":"IS","Value":[2]},'
        '"7FE00010":{"vr":"OB","BulkDataURI":"instances/1/frames"}}'
    )
    encap_items = dicom.read_json(encap_json)
    encap_df, encap_refs = encap_items[0]
    assert encap_df.set_bulk_data(encap_refs[0], b"\x11\x22\x33")
    assert encap_df.set_bulk_data(encap_refs[1], b"\x44\x55")
    assert encap_df.encoded_pixel_frame_view(0).tobytes() == b"\x11\x22\x33"
    assert encap_df.encoded_pixel_frame_view(1).tobytes() == b"\x44\x55"


def test_read_json_missing_vr_falls_back_for_private_and_uid_like_values():
    json_text = (
        '{"00080018":{"Value":["1.2.840.10008.5.1.4.1.1.2"]},'
        '"00083002":{"Value":["1.2.840.10008.1.2.4.80"]},'
        '"00091110":{"Value":["ee51d3c338c9fa07dcdf8fab027dfd6136e21f002cef5916662dce0f614ce43f"]},'
        '"00091112":{"Value":["instance"]}}'
    )

    items = dicom.read_json(json_text)
    df, _refs = items[0]
    assert df["SOPInstanceUID"].vr == dicom.VR.UI
    assert df["SOPInstanceUID"].to_string_view() == "1.2.840.10008.5.1.4.1.1.2"
    assert df["00083002"].vr == dicom.VR.UI
    assert df["00083002"].to_string_view() == "1.2.840.10008.1.2.4.80"
    assert df["00091110"].vr == dicom.VR.UN
    assert df["00091110"].to_utf8_string() == (
        "ee51d3c338c9fa07dcdf8fab027dfd6136e21f002cef5916662dce0f614ce43f"
    )
    assert df["00091112"].vr == dicom.VR.UN
    assert df["00091112"].to_utf8_string() == "instance"
    assert bytes(df["00091112"].value_span()) == b"instance"


def test_read_json_real_metadata_fragment_with_missing_private_vr():
    path = pathlib.Path(__file__).resolve().parents[3] / "sample" / "dicomjson" / "frags" / (
        "dicomweb_studies_1.2.840.113619.2.290.3.3767434740.232.1619607454.466_"
        "series_2.16.840.1.114362.1.12114306.25269253871.637892509.989.444_metadata"
    )
    payload = path.read_bytes()
    items = dicom.read_json(payload)

    assert len(items) > 0
    df, refs = items[0]
    assert df["00091110"].vr == dicom.VR.UN
    assert df["00091112"].to_utf8_string() == "instance"
    assert bytes(df["00091112"].value_span()) == b"instance"
    assert df["00083002"].vr == dicom.VR.UI
    assert df["00083002"].to_string_view() == "1.2.840.10008.1.2.4.80"
    assert any(ref.uri.endswith("/frames") for ref in refs)
    assert all(ref.kind == dicom.JsonBulkTargetKind.element for ref in refs)


def test_read_json_empty_input_reports_non_json_error():
    with pytest.raises(RuntimeError) as exc_info:
        dicom.read_json(b"")

    message = str(exc_info.value)
    assert "not a DICOM JSON stream" in message
    assert "empty input" in message


def test_read_json_gzip_input_reports_non_json_error():
    with pytest.raises(RuntimeError) as exc_info:
        dicom.read_json(b"\x1f\x8b\x08\x00\x00\x00")

    message = str(exc_info.value)
    assert "not a DICOM JSON stream" in message
    assert "invalid JSON byte sequence" in message


def test_read_json_non_json_input_reports_expected_top_level_shape():
    with pytest.raises(RuntimeError) as exc_info:
        dicom.read_json(b"not-json")

    message = str(exc_info.value)
    assert "not a DICOM JSON stream" in message
    assert "top-level JSON object or array" in message
