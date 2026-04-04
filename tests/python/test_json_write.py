import pathlib

import dicomsdl as dicom
import pytest


def _test_file(name: str = "test_le.dcm") -> str:
    return str(pathlib.Path(__file__).resolve().parent.parent / name)


def _populate_fixture_file(df: dicom.DicomFile) -> None:
    assert df.set_value("PatientName", "Doe^Jane")
    assert df.set_value("StudyInstanceUID", "1.2.826.0.1.3680043.10.543.1")
    assert df.set_value("SeriesInstanceUID", "1.2.826.0.1.3680043.10.543.2")
    assert df.set_value("SOPInstanceUID", "1.2.826.0.1.3680043.10.543.3")
    pixel = df.add_dataelement("PixelData", dicom.VR.OB)
    pixel.set_value(b"\x01\x02\x03\x04")


def test_write_json_default_excludes_group_0002():
    df = dicom.read_file(_test_file())

    json_text, bulk_parts = df.write_json()
    assert '"00020010"' not in json_text
    assert '"00020000"' not in json_text
    assert bulk_parts == []

    json_text_meta, _ = df.write_json(include_group_0002=True)
    assert '"00020010"' in json_text_meta
    assert '"00020000"' not in json_text_meta


def test_write_json_uri_mode_returns_borrowed_bulk_memoryviews():
    df = dicom.DicomFile()
    _populate_fixture_file(df)

    json_text, bulk_parts = df.write_json(
        bulk_data="uri",
        bulk_data_threshold=4,
        bulk_data_uri_template="/dicomweb/studies/{study}/series/{series}/instances/{instance}/bulk/{tag}",
    )

    expected_uri = (
        "/dicomweb/studies/1.2.826.0.1.3680043.10.543.1/"
        "series/1.2.826.0.1.3680043.10.543.2/"
        "instances/1.2.826.0.1.3680043.10.543.3/bulk/7FE00010"
    )
    assert f'"7FE00010":{{"vr":"OB","BulkDataURI":"{expected_uri}"}}' in json_text
    assert len(bulk_parts) == 1
    uri, payload, media_type, transfer_syntax_uid = bulk_parts[0]
    assert uri == expected_uri
    assert isinstance(payload, memoryview)
    assert payload.readonly
    assert media_type == "application/octet-stream"
    assert transfer_syntax_uid == "1.2.840.10008.1.2.1"
    assert bytes(payload) == b"\x01\x02\x03\x04"
    assert payload.tobytes() == bytes(df["PixelData"].value_span())


def test_write_json_multiframe_compressed_uses_frame_uris():
    df = dicom.DicomFile()
    assert df.set_value("StudyInstanceUID", "1.2.826.0.1.3680043.10.543.11")
    assert df.set_value("SeriesInstanceUID", "1.2.826.0.1.3680043.10.543.12")
    assert df.set_value("SOPInstanceUID", "1.2.826.0.1.3680043.10.543.13")
    assert df.set_value("TransferSyntaxUID", "1.2.840.10008.1.2.4.80")
    df.add_encoded_pixel_frame(b"\x11\x22\x33")
    df.add_encoded_pixel_frame(b"\x44\x55")
    private_bulk = df.add_dataelement(0x00091001, dicom.VR.OB)
    private_bulk.set_value(b"\x90\x91\x92\x93\x94\x95\x96\x97\x98\x99")

    json_text, bulk_parts = df.write_json(
        bulk_data="uri",
        bulk_data_threshold=8,
        bulk_data_uri_template="/dicomweb/studies/{study}/series/{series}/instances/{instance}/bulk/{tag}",
        pixel_data_uri_template="/dicomweb/studies/{study}/series/{series}/instances/{instance}/frames",
    )

    base_uri = (
        "/dicomweb/studies/1.2.826.0.1.3680043.10.543.11/"
        "series/1.2.826.0.1.3680043.10.543.12/"
        "instances/1.2.826.0.1.3680043.10.543.13/frames"
    )
    assert f'"7FE00010":{{"vr":"OB","BulkDataURI":"{base_uri}"}}' in json_text
    assert (
        '"00091001":{"vr":"OB","BulkDataURI":"'
        "/dicomweb/studies/1.2.826.0.1.3680043.10.543.11/"
        "series/1.2.826.0.1.3680043.10.543.12/"
        "instances/1.2.826.0.1.3680043.10.543.13/bulk/00091001\"}"
    ) in json_text
    bulk_parts_by_uri = {uri: (payload, media_type, transfer_syntax_uid) for uri, payload, media_type, transfer_syntax_uid in bulk_parts}
    assert set(bulk_parts_by_uri) == {
        f"{base_uri}/1",
        f"{base_uri}/2",
        "/dicomweb/studies/1.2.826.0.1.3680043.10.543.11/"
        "series/1.2.826.0.1.3680043.10.543.12/"
        "instances/1.2.826.0.1.3680043.10.543.13/bulk/00091001",
    }
    assert bytes(bulk_parts_by_uri[f"{base_uri}/1"][0]) == b"\x11\x22\x33"
    assert bytes(bulk_parts_by_uri[f"{base_uri}/2"][0]) == b"\x44\x55"
    assert bulk_parts_by_uri[f"{base_uri}/1"][1] == "image/jls"
    assert bulk_parts_by_uri[f"{base_uri}/1"][2] == "1.2.840.10008.1.2.4.80"
    assert bulk_parts_by_uri[f"{base_uri}/2"][1] == "image/jls"
    assert bulk_parts_by_uri[f"{base_uri}/2"][2] == "1.2.840.10008.1.2.4.80"
    assert (
        bytes(
            bulk_parts_by_uri[
                "/dicomweb/studies/1.2.826.0.1.3680043.10.543.11/"
                "series/1.2.826.0.1.3680043.10.543.12/"
                "instances/1.2.826.0.1.3680043.10.543.13/bulk/00091001"
            ][0]
        )
        == b"\x90\x91\x92\x93\x94\x95\x96\x97\x98\x99"
    )
    assert (
        bulk_parts_by_uri[
            "/dicomweb/studies/1.2.826.0.1.3680043.10.543.11/"
            "series/1.2.826.0.1.3680043.10.543.12/"
            "instances/1.2.826.0.1.3680043.10.543.13/bulk/00091001"
        ][1]
        == "application/octet-stream"
    )
    assert (
        bulk_parts_by_uri[
            "/dicomweb/studies/1.2.826.0.1.3680043.10.543.11/"
            "series/1.2.826.0.1.3680043.10.543.12/"
            "instances/1.2.826.0.1.3680043.10.543.13/bulk/00091001"
        ][2]
        == ""
    )


def test_write_json_multiframe_native_uses_one_bulk_uri():
    df = dicom.DicomFile()
    assert df.set_value("StudyInstanceUID", "1.2.826.0.1.3680043.10.543.31")
    assert df.set_value("SeriesInstanceUID", "1.2.826.0.1.3680043.10.543.32")
    assert df.set_value("SOPInstanceUID", "1.2.826.0.1.3680043.10.543.33")
    assert df.set_value("Rows", 2)
    assert df.set_value("Columns", 1)
    assert df.set_value("SamplesPerPixel", 1)
    assert df.set_value("BitsAllocated", 16)
    assert df.set_value("BitsStored", 16)
    assert df.set_value("HighBit", 15)
    assert df.set_value("PixelRepresentation", 0)
    assert df.set_value("NumberOfFrames", 2)
    assert df.set_value("PhotometricInterpretation", "MONOCHROME2")
    assert df.set_value("TransferSyntaxUID", "1.2.840.10008.1.2.1")
    pixel = df.add_dataelement("PixelData", dicom.VR.OW)
    pixel.set_value(b"\x01\x00\x02\x00\x03\x00\x04\x00")

    json_text, bulk_parts = df.write_json(
        bulk_data="uri",
        bulk_data_threshold=4,
        pixel_data_uri_template="/dicomweb/studies/{study}/series/{series}/instances/{instance}/frames",
    )

    base_uri = (
        "/dicomweb/studies/1.2.826.0.1.3680043.10.543.31/"
        "series/1.2.826.0.1.3680043.10.543.32/"
        "instances/1.2.826.0.1.3680043.10.543.33/frames"
    )
    assert f'"7FE00010":{{"vr":"OW","BulkDataURI":"{base_uri}"}}' in json_text
    bulk_parts_by_uri = {uri: (payload, media_type, transfer_syntax_uid) for uri, payload, media_type, transfer_syntax_uid in bulk_parts}
    assert set(bulk_parts_by_uri) == {base_uri}
    assert bytes(bulk_parts_by_uri[base_uri][0]) == b"\x01\x00\x02\x00\x03\x00\x04\x00"
    assert bulk_parts_by_uri[base_uri][1] == "application/octet-stream"
    assert bulk_parts_by_uri[base_uri][2] == "1.2.840.10008.1.2.1"


def test_write_json_nested_bulk_expands_tag_to_dotted_path():
    df = dicom.DicomFile()
    assert df.set_value("StudyInstanceUID", "1.2.826.0.1.3680043.10.543.41")
    assert df.set_value("SeriesInstanceUID", "1.2.826.0.1.3680043.10.543.42")
    assert df.set_value("SOPInstanceUID", "1.2.826.0.1.3680043.10.543.43")

    seq = df.add_dataelement(0x22002200, dicom.VR.SQ).sequence
    item0 = seq.add_dataset()
    item1 = seq.add_dataset()
    item0.add_dataelement(0x12340012, dicom.VR.OB).set_value(b"\x01\x02\x03\x04")
    item1.add_dataelement(0x12340012, dicom.VR.OB).set_value(b"\x05\x06\x07\x08")

    json_text, bulk_parts = df.write_json(
        bulk_data="uri",
        bulk_data_threshold=4,
        bulk_data_uri_template="/dicomweb/studies/{study}/series/{series}/instances/{instance}/bulk/{tag}",
    )

    uri0 = (
        "/dicomweb/studies/1.2.826.0.1.3680043.10.543.41/"
        "series/1.2.826.0.1.3680043.10.543.42/"
        "instances/1.2.826.0.1.3680043.10.543.43/bulk/22002200.0.12340012"
    )
    uri1 = (
        "/dicomweb/studies/1.2.826.0.1.3680043.10.543.41/"
        "series/1.2.826.0.1.3680043.10.543.42/"
        "instances/1.2.826.0.1.3680043.10.543.43/bulk/22002200.1.12340012"
    )
    assert uri0 in json_text
    assert uri1 in json_text
    assert {uri for uri, *_ in bulk_parts} == {uri0, uri1}


def test_dataset_write_json_missing_template_placeholder_raises():
    ds = dicom.DataSet()
    pixel = ds.add_dataelement("PixelData", dicom.VR.OB)
    pixel.set_value(b"\x01\x02\x03\x04")

    with pytest.raises(Exception):
        ds.write_json(
            bulk_data="uri",
            bulk_data_threshold=4,
            bulk_data_uri_template="/dicomweb/studies/{study}/bulk/{tag}",
        )


def test_write_json_frame_template_without_generic_bulk_template_raises_helpful_error():
    df = dicom.DicomFile()
    assert df.set_value("StudyInstanceUID", "1.2.826.0.1.3680043.10.543.21")
    assert df.set_value("SeriesInstanceUID", "1.2.826.0.1.3680043.10.543.22")
    assert df.set_value("SOPInstanceUID", "1.2.826.0.1.3680043.10.543.23")
    df.add_encoded_pixel_frame(b"\x11\x22\x33")
    df.add_encoded_pixel_frame(b"\x44\x55")
    private_bulk = df.add_dataelement(0x00091001, dicom.VR.OB)
    private_bulk.set_value(b"\x90\x91\x92\x93\x94\x95\x96\x97\x98\x99")

    with pytest.raises(RuntimeError) as exc_info:
        df.write_json(
            bulk_data="uri",
            bulk_data_threshold=8,
            bulk_data_uri_template="/dicomweb/studies/{study}/series/{series}/instances/{instance}/frames",
        )

    message = str(exc_info.value)
    assert "duplicate BulkDataURI generated" in message
    assert "pixel_data_uri_template" in message
    assert "{tag}" in message
