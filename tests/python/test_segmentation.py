from __future__ import annotations

import os

import numpy as np
import pytest

import dicomsdl as dicom


def _set(df: dicom.DicomFile, key: str, value: object) -> None:
    assert df.set_value(key, value) is True


def _set_vr(df: dicom.DicomFile, key: str, vr: dicom.VR, value: object) -> None:
    assert df.set_value(key, vr, value) is True


def _populate_common_binary_seg(df: dicom.DicomFile) -> None:
    seg_uid = dicom.uid_from_keyword("SegmentationStorage").value
    sop_uid = "1.2.826.0.1.3680043.10.543.400"

    _set(df, "StudyInstanceUID", "1.2.826.0.1.3680043.10.543.100")
    _set(df, "SeriesInstanceUID", "1.2.826.0.1.3680043.10.543.200")
    _set(df, "SOPClassUID", seg_uid)
    _set(df, "SOPInstanceUID", sop_uid)
    _set(df, "MediaStorageSOPClassUID", seg_uid)
    _set(df, "MediaStorageSOPInstanceUID", sop_uid)
    _set(df, "TransferSyntaxUID", dicom.uid_from_keyword("ExplicitVRLittleEndian").value)
    _set(df, "Modality", "SEG")
    _set(df, "FrameOfReferenceUID", "1.2.826.0.1.3680043.10.543.42")
    _set(df, "SegmentationType", "BINARY")
    _set(df, "Rows", 2)
    _set(df, "Columns", 8)
    _set(df, "SamplesPerPixel", 1)
    _set(df, "PhotometricInterpretation", "MONOCHROME2")
    _set(df, "PixelRepresentation", 0)
    _set(df, "NumberOfFrames", 2)
    _set(df, "BitsAllocated", 1)
    _set(df, "BitsStored", 1)
    _set(df, "HighBit", 0)

    _set(
        df,
        "SharedFunctionalGroupsSequence.0.PixelMeasuresSequence.0.PixelSpacing",
        [1.5, 2.5],
    )
    _set(
        df,
        "SharedFunctionalGroupsSequence.0.PixelMeasuresSequence.0.SliceThickness",
        3,
    )
    _set(
        df,
        "SharedFunctionalGroupsSequence.0.PlaneOrientationSequence.0."
        "ImageOrientationPatient",
        [1, 0, 0, 0, 1, 0],
    )


def _populate_segment(
    df: dicom.DicomFile, ordinal: int, number: int, label: str
) -> None:
    base = f"SegmentSequence.{ordinal}."
    _set(df, base + "SegmentNumber", number)
    _set(df, base + "SegmentLabel", label)
    _set(df, base + "SegmentDescription", f"{label} description")
    _set(df, base + "SegmentAlgorithmType", "AUTOMATIC")
    _set(df, base + "SegmentAlgorithmName", "python-test")
    _set(df, base + "RecommendedDisplayCIELabValue", [1000 + ordinal, 2000, 3000])
    _set(df, base + "SegmentedPropertyCategoryCodeSequence.0.CodeValue", "T-D0050")
    _set(df, base + "SegmentedPropertyCategoryCodeSequence.0.CodingSchemeDesignator", "SRT")
    _set(df, base + "SegmentedPropertyCategoryCodeSequence.0.CodeMeaning", "Tissue")


def _populate_frame(
    df: dicom.DicomFile, frame_index: int, segment_number: int, z_position: float
) -> None:
    base = f"PerFrameFunctionalGroupsSequence.{frame_index}."
    _set(
        df,
        base + "SegmentIdentificationSequence.0.ReferencedSegmentNumber",
        segment_number,
    )
    _set(df, base + "PlanePositionSequence.0.ImagePositionPatient", [0, 0, z_position])
    _set(
        df,
        base
        + "DerivationImageSequence.0.SourceImageSequence.0.ReferencedSOPClassUID",
        "1.2.840.10008.5.1.4.1.1.2",
    )
    _set(
        df,
        base
        + "DerivationImageSequence.0.SourceImageSequence.0.ReferencedSOPInstanceUID",
        f"1.2.826.0.1.3680043.10.543.{300 + frame_index}",
    )
    _set(
        df,
        base + "DerivationImageSequence.0.SourceImageSequence.0.ReferencedFrameNumber",
        [frame_index + 1, frame_index + 2],
    )


def _make_binary_seg() -> dicom.DicomFile:
    df = dicom.DicomFile()
    _populate_common_binary_seg(df)
    _populate_segment(df, 0, 1, "First")
    _populate_segment(df, 1, 2, "Second")
    _populate_frame(df, 0, 1, 10.0)
    _populate_frame(df, 1, 2, 20.0)
    _set_vr(df, "PixelData", dicom.VR.OB, bytes([0x55, 0x0F, 0x80, 0x33]))
    return df


def _make_fractional_seg() -> dicom.DicomFile:
    df = dicom.DicomFile()
    _populate_common_binary_seg(df)
    _set(df, "SeriesInstanceUID", "1.2.826.0.1.3680043.10.543.201")
    _set(df, "SOPInstanceUID", "1.2.826.0.1.3680043.10.543.401")
    _set(df, "MediaStorageSOPInstanceUID", "1.2.826.0.1.3680043.10.543.401")
    _set(df, "SegmentationType", "FRACTIONAL")
    _set(df, "SegmentationFractionalType", "PROBABILITY")
    _set(df, "MaximumFractionalValue", 255)
    _set(df, "Rows", 2)
    _set(df, "Columns", 2)
    _set(df, "NumberOfFrames", 1)
    _set(df, "BitsAllocated", 8)
    _set(df, "BitsStored", 8)
    _set(df, "HighBit", 7)
    _populate_segment(df, 0, 1, "Probability")
    _populate_frame(df, 0, 1, 10.0)
    _set_vr(df, "PixelData", dicom.VR.OB, bytes([0, 128, 255, 64]))
    return df


def test_segmentation_from_dicomfile_decodes_binary_frames() -> None:
    df = _make_binary_seg()

    assert dicom.seg.is_segmentation_storage(df)
    seg = dicom.seg.from_dicomfile(df)

    assert seg.is_valid
    assert seg.segmentation_type is dicom.seg.SegmentationType.binary
    assert seg.fractional_type is dicom.seg.SegmentationFractionalType.none
    assert seg.frame_of_reference_uid == "1.2.826.0.1.3680043.10.543.42"
    assert seg.rows == 2
    assert seg.columns == 8
    assert seg.segment_count == 2
    assert seg.frame_count == 2

    first = seg.segments[0]
    assert first.number == 1
    assert first.label == "First"
    assert first.description == "First description"
    assert first.algorithm_type is dicom.seg.SegmentAlgorithmType.automatic
    assert first.algorithm_name == "python-test"
    assert first.property_category.value == "T-D0050"
    assert first.property_category.scheme_designator == "SRT"
    assert first.recommended_display_cielab == (1000, 2000, 3000)
    assert first.dataset.get_value("SegmentLabel") == "First"

    second = seg.segment_by_number(2)
    assert second is not None
    assert second.label == "Second"
    assert seg.segment_by_number(99) is None

    frame0 = seg.frames[0]
    assert frame0.index == 0
    assert frame0.referenced_segment_number == 1
    assert frame0.image_position_patient == (0.0, 0.0, 10.0)
    assert frame0.image_orientation_patient == (1.0, 0.0, 0.0, 0.0, 1.0, 0.0)
    assert frame0.pixel_spacing == (1.5, 2.5)
    assert frame0.slice_thickness == 3.0
    assert frame0.per_frame_functional_groups_item["SegmentIdentificationSequence"]

    refs = frame0.source_images
    assert len(refs) == 1
    assert refs[0].sop_class_uid == "1.2.840.10008.5.1.4.1.1.2"
    assert refs[0].sop_instance_uid == "1.2.826.0.1.3680043.10.543.300"
    assert refs[0].referenced_frame_numbers == [1, 2]

    expected0 = np.array(
        [[1, 0, 1, 0, 1, 0, 1, 0], [1, 1, 1, 1, 0, 0, 0, 0]],
        dtype=np.uint8,
    )
    expected1 = np.array(
        [[0, 0, 0, 0, 0, 0, 0, 1], [1, 1, 0, 0, 1, 1, 0, 0]],
        dtype=np.uint8,
    )
    np.testing.assert_array_equal(seg.to_array(0), expected0)
    np.testing.assert_array_equal(frame0.to_array(), expected0)
    np.testing.assert_array_equal(seg.frames_for_segment(2)[0].to_array(), expected1)
    assert [segment.number for segment in seg.segments] == [1, 2]
    assert [frame.index for frame in seg.frames] == [0, 1]
    assert [ref.sop_instance_uid for ref in refs] == [
        "1.2.826.0.1.3680043.10.543.300"
    ]

    assert seg.decode_frame(0) == expected0.tobytes()
    out = np.empty((2, 8), dtype=np.uint8)
    returned = seg.decode_frame_into(1, out)
    assert returned is out
    np.testing.assert_array_equal(out, expected1)


def test_segmentation_from_file_and_from_bytes(tmp_path) -> None:
    df = _make_binary_seg()
    path = tmp_path / "seg.dcm"
    df.write_file(path)

    from_file = dicom.seg.from_file(path)
    assert from_file.segment_count == 2
    assert from_file.frames_for_segment(1)[0].referenced_segment_number == 1

    source = bytearray(path.read_bytes())
    from_bytes = dicom.seg.from_bytes(source, copy=False)
    assert from_bytes.frame_count == 2
    assert from_bytes.decode_frame(-1) == from_file.decode_frame(1)

    borrowed_frame = dicom.seg.from_bytes(bytearray(path.read_bytes()), copy=False).frames[1]
    assert borrowed_frame.decode_frame() == from_file.decode_frame(1)


def test_fractional_segmentation_returns_raw_uint8_samples() -> None:
    seg = dicom.seg.from_dicomfile(_make_fractional_seg())

    assert seg.segmentation_type is dicom.seg.SegmentationType.fractional
    assert seg.fractional_type is dicom.seg.SegmentationFractionalType.probability
    assert seg.maximum_fractional_value == 255
    assert seg.rows == 2
    assert seg.columns == 2
    assert seg.segment_count == 1
    assert seg.frame_count == 1

    raw = seg.to_array(0)
    expected = np.array([[0, 128], [255, 64]], dtype=np.uint8)
    np.testing.assert_array_equal(raw, expected)
    scaled = raw.astype(np.float32) / seg.maximum_fractional_value
    np.testing.assert_allclose(
        scaled,
        np.array([[0.0, 128.0 / 255.0], [1.0, 64.0 / 255.0]], dtype=np.float32),
    )


def test_unsupported_segmentation_errors_are_explicit() -> None:
    labelmap = _make_binary_seg()
    _set(labelmap, "SegmentationType", "LABELMAP")
    with pytest.raises(Exception, match="LABELMAP SEG"):
        dicom.seg.from_dicomfile(labelmap)

    labelmap_sop = _make_binary_seg()
    labelmap_uid = dicom.uid_from_keyword("LabelMapSegmentationStorage").value
    _set(labelmap_sop, "SOPClassUID", labelmap_uid)
    _set(labelmap_sop, "MediaStorageSOPClassUID", labelmap_uid)
    with pytest.raises(Exception, match="LABELMAP SEG"):
        dicom.seg.from_dicomfile(labelmap_sop)

    explicit_uid = dicom.uid_from_keyword("ExplicitVRLittleEndian").value.encode("ascii")
    rle_uid = dicom.uid_from_keyword("RLELossless").value.encode("ascii")
    compressed_bytes = bytearray(_make_binary_seg().write_bytes())
    assert compressed_bytes.count(explicit_uid) >= 1
    compressed_bytes = compressed_bytes.replace(explicit_uid, rle_uid, 1)
    seg = dicom.seg.from_bytes(compressed_bytes)
    with pytest.raises(Exception, match="compressed/encapsulated BINARY SEG"):
        seg.decode_frame(0)

    short_pixel_data = _make_binary_seg()
    _set_vr(short_pixel_data, "PixelData", dicom.VR.OB, b"\x00")
    seg = dicom.seg.from_dicomfile(short_pixel_data)
    with pytest.raises(Exception, match="PixelData size mismatch"):
        seg.decode_frame(0)

    missing_segment_sequence = _make_binary_seg()
    missing_segment_sequence.remove_dataelement("SegmentSequence")
    with pytest.raises(Exception, match="SegmentSequence"):
        dicom.seg.from_dicomfile(missing_segment_sequence)

    missing_per_frame_sequence = _make_binary_seg()
    missing_per_frame_sequence.remove_dataelement("PerFrameFunctionalGroupsSequence")
    with pytest.raises(Exception, match="PerFrameFunctionalGroupsSequence"):
        dicom.seg.from_dicomfile(missing_per_frame_sequence)

    missing_geometry = _make_binary_seg()
    missing_geometry.remove_dataelement("SharedFunctionalGroupsSequence")
    with pytest.raises(Exception, match="SharedFunctionalGroupsSequence"):
        dicom.seg.from_dicomfile(missing_geometry)


def test_optional_local_seg_sample_regression() -> None:
    path = os.environ.get("DICOMSDL_SEG_SAMPLE_PATH")
    if not path:
        pytest.skip("set DICOMSDL_SEG_SAMPLE_PATH to enable local SEG sample regression")

    seg = dicom.seg.from_file(path)
    assert seg.is_valid
    assert seg.segment_count > 0
    assert seg.frame_count > 0
    assert seg.rows > 0
    assert seg.columns > 0
    assert seg.frame_of_reference_uid
    decoded = seg.to_array(0)
    assert decoded.shape == (seg.rows, seg.columns)
    assert decoded.dtype == np.uint8
