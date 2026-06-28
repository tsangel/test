from __future__ import annotations

import os
from pathlib import Path

import numpy as np
import pytest

import dicomsdl as dicom

REPO_ROOT = Path(__file__).resolve().parents[2]


def _set(df: dicom.DicomFile, key: str, value: object) -> None:
    assert df.set_value(key, value) is True


def _set_vr(df: dicom.DicomFile, key: str, vr: dicom.VR, value: object) -> None:
    assert df.set_value(key, vr, value) is True


def _seg_from_synthetic(df: dicom.DicomFile) -> dicom.seg.Segmentation:
    return dicom.seg.read_bytes(df.write_bytes())


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


def _populate_labelmap_frame(
    df: dicom.DicomFile, frame_index: int, z_position: float
) -> None:
    base = f"PerFrameFunctionalGroupsSequence.{frame_index}."
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
        f"1.2.826.0.1.3680043.10.543.{500 + frame_index}",
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


def _make_labelmap_seg8(pixel_data: bytes | None = None) -> dicom.DicomFile:
    df = dicom.DicomFile()
    _populate_common_binary_seg(df)
    labelmap_uid = dicom.uid_from_keyword("LabelMapSegmentationStorage").value
    _set(df, "SOPClassUID", labelmap_uid)
    _set(df, "MediaStorageSOPClassUID", labelmap_uid)
    _set(df, "SegmentationType", "LABELMAP")
    _set(df, "Rows", 2)
    _set(df, "Columns", 3)
    _set(df, "NumberOfFrames", 2)
    _set(df, "BitsAllocated", 8)
    _set(df, "BitsStored", 8)
    _set(df, "HighBit", 7)
    _set(df, "PixelPaddingValue", 0)
    _set(df, "SegmentsOverlap", "NO")
    _populate_segment(df, 0, 0, "Background")
    _populate_segment(df, 1, 1, "One")
    _populate_segment(df, 2, 2, "Two")
    _populate_segment(df, 3, 7, "Absent")
    _populate_labelmap_frame(df, 0, 10.0)
    _populate_labelmap_frame(df, 1, 20.0)
    _set_vr(
        df,
        "PixelData",
        dicom.VR.OB,
        pixel_data
        if pixel_data is not None
        else bytes([0, 1, 2, 2, 0, 1, 0, 0, 2, 0, 0, 0]),
    )
    return df


def _make_labelmap_seg16() -> dicom.DicomFile:
    df = dicom.DicomFile()
    _populate_common_binary_seg(df)
    labelmap_uid = dicom.uid_from_keyword("LabelMapSegmentationStorage").value
    _set(df, "SOPClassUID", labelmap_uid)
    _set(df, "MediaStorageSOPClassUID", labelmap_uid)
    _set(df, "SegmentationType", "LABELMAP")
    _set(df, "Rows", 2)
    _set(df, "Columns", 2)
    _set(df, "NumberOfFrames", 1)
    _set(df, "BitsAllocated", 16)
    _set(df, "BitsStored", 16)
    _set(df, "HighBit", 15)
    _set(df, "PixelPaddingValue", 0)
    _populate_segment(df, 0, 0, "Background")
    _populate_segment(df, 1, 1, "One")
    _populate_segment(df, 2, 300, "Three Hundred")
    _populate_labelmap_frame(df, 0, 10.0)
    values = np.array([[0, 1], [300, 300]], dtype=np.uint16)
    _set_vr(df, "PixelData", dicom.VR.OW, values.tobytes())
    return df


def _make_labelmap_seg8_odd_frame(pixel_data: bytes) -> dicom.DicomFile:
    df = dicom.DicomFile()
    _populate_common_binary_seg(df)
    labelmap_uid = dicom.uid_from_keyword("LabelMapSegmentationStorage").value
    _set(df, "SOPClassUID", labelmap_uid)
    _set(df, "MediaStorageSOPClassUID", labelmap_uid)
    _set(df, "SegmentationType", "LABELMAP")
    _set(df, "Rows", 1)
    _set(df, "Columns", 3)
    _set(df, "NumberOfFrames", 1)
    _set(df, "BitsAllocated", 8)
    _set(df, "BitsStored", 8)
    _set(df, "HighBit", 7)
    _set(df, "PixelPaddingValue", 0)
    _populate_segment(df, 0, 0, "Background")
    _populate_segment(df, 1, 1, "One")
    _populate_segment(df, 2, 2, "Two")
    _populate_labelmap_frame(df, 0, 10.0)
    _set_vr(df, "PixelData", dicom.VR.OB, pixel_data)
    return df


def test_segmentation_read_bytes_decodes_binary_frames() -> None:
    df = _make_binary_seg()

    assert dicom.seg.is_segmentation_storage(df)
    seg = _seg_from_synthetic(df)

    assert seg.is_valid
    assert seg.segmentation_type is dicom.seg.SegmentationType.binary
    assert seg.fractional_type is dicom.seg.SegmentationFractionalType.none
    assert seg.segments_overlap is dicom.seg.SegmentsOverlap.undefined
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
    assert frame0.present_segment_numbers == (1,)
    np.testing.assert_array_equal(seg.mask_for_segment(0, 1), expected0)
    np.testing.assert_array_equal(
        seg.mask_for_segment(0, 2), np.zeros((2, 8), dtype=np.uint8)
    )
    assert [segment.number for segment in seg.segments] == [1, 2]
    assert [frame.index for frame in seg.frames] == [0, 1]
    assert [ref.sop_instance_uid for ref in refs] == [
        "1.2.826.0.1.3680043.10.543.300"
    ]

    frame_iter = iter(seg.frames)
    assert frame_iter is iter(frame_iter)
    assert next(frame_iter).index == 0
    assert next(frame_iter).index == 1
    with pytest.raises(StopIteration):
        next(frame_iter)

    assert seg.decode_frame(0) == expected0.tobytes()
    out = np.empty((2, 8), dtype=np.uint8)
    returned = seg.decode_frame_into(1, out)
    assert returned is out
    np.testing.assert_array_equal(out, expected1)


def test_binary_segmentation_builds_packed_label_volume() -> None:
    seg = _seg_from_synthetic(_make_binary_seg())

    packed = seg.build_binary_label_volume([(0, 0), (1, 1)], slices=2)

    assert packed.shape == (2, 2, 8)
    assert packed.source_dicom_segment_by_label_id == [0, 1, 2]
    assert packed.single_label_code_end == 2
    assert packed.overlap_entry_count == 0
    assert packed.label_id_for_segment_number(1) == 1
    assert packed.label_id_for_segment_number(2) == 2
    assert packed.label_id_for_segment_number(99) is None
    assert packed.label_set(0) == ()
    assert packed.label_set(1) == (1,)
    assert packed.label_codes_for_label_id(1) == (1,)

    expected0 = np.array(
        [[1, 0, 1, 0, 1, 0, 1, 0], [1, 1, 1, 1, 0, 0, 0, 0]],
        dtype=np.uint16,
    )
    expected1 = np.array(
        [[0, 0, 0, 0, 0, 0, 0, 2], [2, 2, 0, 0, 2, 2, 0, 0]],
        dtype=np.uint16,
    )
    expected_volume = np.stack([expected0, expected1])
    assert packed.label_volume.dtype == np.uint16
    np.testing.assert_array_equal(packed.label_volume, expected_volume)

    mask1 = np.zeros((2, 2, 8), dtype=np.uint8)
    mask1[0] = expected0.astype(np.uint8)
    np.testing.assert_array_equal(packed.restore_mask_for_segment(1), mask1)

    mask2 = np.zeros((2, 2, 8), dtype=np.uint8)
    mask2[1] = (expected1 == 2).astype(np.uint8)
    np.testing.assert_array_equal(packed.restore_mask_for_label_id(2), mask2)

    with pytest.raises(Exception, match="SegmentNumber"):
        packed.restore_mask_for_segment(99)

    out = np.empty((2, 2, 8), dtype=np.uint16)
    packed_into = seg.build_binary_label_volume_into(
        out, [(0, 0), (1, 1)], slices=2
    )
    np.testing.assert_array_equal(out, expected_volume)
    np.testing.assert_array_equal(packed_into.label_volume, expected_volume)
    assert np.shares_memory(packed_into.label_volume, out)
    np.testing.assert_array_equal(packed_into.restore_mask_for_segment(2), mask2)

    with pytest.raises(Exception, match="uint16"):
        seg.build_binary_label_volume_into(
            np.empty((2, 2, 8), dtype=np.int16), [(0, 0), (1, 1)], slices=2
        )

    with pytest.raises(Exception, match="volume shape"):
        seg.build_binary_label_volume_into(
            np.empty((2, 2, 7), dtype=np.uint16), [(0, 0), (1, 1)], slices=2
        )


def test_segmentation_read_file_and_read_bytes(tmp_path) -> None:
    df = _make_binary_seg()
    path = tmp_path / "seg.dcm"
    df.write_file(path)

    assert not hasattr(dicom.seg, "from_dicomfile")
    assert not hasattr(dicom.seg, "from_file")
    assert not hasattr(dicom.seg, "from_bytes")

    read_file = dicom.seg.read_file(path)
    assert read_file.segment_count == 2
    assert read_file.frames_for_segment(1)[0].referenced_segment_number == 1

    source = bytearray(path.read_bytes())
    read_bytes = dicom.seg.read_bytes(source, copy=False)
    assert read_bytes.frame_count == 2
    assert read_bytes.decode_frame(-1) == read_file.decode_frame(1)

    borrowed_frame = dicom.seg.read_bytes(bytearray(path.read_bytes()), copy=False).frames[1]
    assert borrowed_frame.decode_frame() == read_file.decode_frame(1)


def test_segmentation_python_api_keeps_only_direct_ownership_paths() -> None:
    assert not hasattr(dicom.seg, "from_dicomfile")
    assert not hasattr(dicom.seg, "from_file")
    assert not hasattr(dicom.seg, "from_bytes")


def test_fractional_segmentation_returns_raw_uint8_samples() -> None:
    seg = _seg_from_synthetic(_make_fractional_seg())

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
    np.testing.assert_array_equal(
        seg.mask_for_segment(0, 1, fractional_threshold=0.5),
        np.array([[0, 1], [1, 0]], dtype=np.uint8),
    )
    with pytest.raises(Exception, match="fractional_threshold"):
        seg.mask_for_segment(0, 1, fractional_threshold=1.5)
    scaled = raw.astype(np.float32) / seg.maximum_fractional_value
    np.testing.assert_allclose(
        scaled,
        np.array([[0.0, 128.0 / 255.0], [1.0, 64.0 / 255.0]], dtype=np.float32),
    )


def test_unsupported_segmentation_errors_are_explicit() -> None:
    labelmap = _make_binary_seg()
    _set(labelmap, "SegmentationType", "LABELMAP")
    with pytest.raises(Exception, match="Label Map|LABELMAP|Segmentation Storage"):
        _seg_from_synthetic(labelmap)

    labelmap_sop = _make_binary_seg()
    labelmap_uid = dicom.uid_from_keyword("LabelMapSegmentationStorage").value
    _set(labelmap_sop, "SOPClassUID", labelmap_uid)
    _set(labelmap_sop, "MediaStorageSOPClassUID", labelmap_uid)
    with pytest.raises(Exception, match="SegmentationType=LABELMAP"):
        _seg_from_synthetic(labelmap_sop)

    explicit_uid = dicom.uid_from_keyword("ExplicitVRLittleEndian").value.encode("ascii")
    rle_uid = dicom.uid_from_keyword("RLELossless").value.encode("ascii")
    compressed_bytes = bytearray(_make_binary_seg().write_bytes())
    assert compressed_bytes.count(explicit_uid) >= 1
    compressed_bytes = compressed_bytes.replace(explicit_uid, rle_uid, 1)
    seg = dicom.seg.read_bytes(compressed_bytes)
    with pytest.raises(Exception, match="compressed BINARY SEG"):
        seg.decode_frame(0)

    short_pixel_data = _make_binary_seg()
    _set_vr(short_pixel_data, "PixelData", dicom.VR.OB, b"\x00")
    seg = _seg_from_synthetic(short_pixel_data)
    with pytest.raises(Exception, match="PixelData size mismatch"):
        seg.decode_frame(0)

    missing_segment_sequence = _make_binary_seg()
    missing_segment_sequence.remove_dataelement("SegmentSequence")
    with pytest.raises(Exception, match="SegmentSequence"):
        _seg_from_synthetic(missing_segment_sequence)

    missing_per_frame_sequence = _make_binary_seg()
    missing_per_frame_sequence.remove_dataelement("PerFrameFunctionalGroupsSequence")
    with pytest.raises(Exception, match="PerFrameFunctionalGroupsSequence"):
        _seg_from_synthetic(missing_per_frame_sequence)

    missing_geometry = _make_binary_seg()
    missing_geometry.remove_dataelement("SharedFunctionalGroupsSequence")
    with pytest.raises(Exception, match="SharedFunctionalGroupsSequence"):
        _seg_from_synthetic(missing_geometry)


def test_labelmap_segmentation_storage_decodes_and_indexes_labels() -> None:
    df = _make_labelmap_seg8()

    assert not dicom.seg.is_segmentation_storage(df)
    assert dicom.seg.is_labelmap_segmentation_storage(df)
    assert dicom.seg.is_any_segmentation_storage(df)

    seg = _seg_from_synthetic(df)
    assert seg.segmentation_type is dicom.seg.SegmentationType.labelmap
    assert seg.segments_overlap is dicom.seg.SegmentsOverlap.no
    assert seg.labelmap_bits_allocated == 8
    assert seg.segment_count == 4
    assert seg.frame_count == 2
    assert seg.segment_by_number(0).label == "Background"
    assert repr(seg.frames[0]) == "SegmentFrame(index=0, type=labelmap)"
    with pytest.raises(Exception, match="referenced_segment_number"):
        _ = seg.frames[0].referenced_segment_number

    expected0 = np.array([[0, 1, 2], [2, 0, 1]], dtype=np.uint8)
    expected1 = np.array([[0, 0, 2], [0, 0, 0]], dtype=np.uint8)
    np.testing.assert_array_equal(seg.to_array(0), expected0)
    np.testing.assert_array_equal(seg.frames[1].to_array(), expected1)
    assert seg.present_segment_numbers(0) == (1, 2)
    assert seg.frames[0].present_segment_numbers == (1, 2)
    assert seg.present_segment_numbers(1) == (2,)

    out = bytearray(6)
    returned = seg.decode_frame_into(0, out)
    assert returned is out
    assert bytes(out) == expected0.tobytes()

    np.testing.assert_array_equal(
        seg.mask_for_segment(0, 1),
        np.array([[0, 1, 0], [0, 0, 1]], dtype=np.uint8),
    )
    np.testing.assert_array_equal(
        seg.frames[0].mask_for_segment(2),
        np.array([[0, 0, 1], [1, 0, 0]], dtype=np.uint8),
    )
    np.testing.assert_array_equal(
        seg.mask_for_segment(0, 7), np.zeros((2, 3), dtype=np.uint8)
    )
    np.testing.assert_array_equal(
        seg.mask_for_segment(0, 0),
        np.array([[1, 0, 0], [0, 1, 0]], dtype=np.uint8),
    )
    with pytest.raises(Exception, match="not present"):
        seg.mask_for_segment(0, 7, error_when_not_present_in_frame=True)
    with pytest.raises(Exception, match="SegmentSequence"):
        seg.mask_for_segment(0, 99)

    assert [frame.index for frame in seg.frames_for_segment(2)] == [0, 1]
    assert len(seg.frames_for_segment(0)) == 0
    assert seg.segment_frame_count(0) == 0
    assert len(seg.frames_for_segment(7)) == 0
    assert seg.segment_frame_count(7) == 0
    assert len(seg.frames_for_segment(99)) == 0
    assert seg.segment_frame_count(99) == 0
    seg.validate_label_values()


def test_lossless_encapsulated_seg_roundtrip_preserves_public_api() -> None:
    transfer_syntaxes = (
        "EncapsulatedUncompressedExplicitVRLittleEndian",
        "RLELossless",
    )

    for transfer_syntax in transfer_syntaxes:
        fractional = dicom.seg.read_bytes(
            _make_fractional_seg().write_bytes_with_transfer_syntax(transfer_syntax)
        )
        np.testing.assert_array_equal(
            fractional.to_array(0),
            np.array([[0, 128], [255, 64]], dtype=np.uint8),
        )
        np.testing.assert_array_equal(
            fractional.mask_for_segment(0, 1),
            np.array([[0, 1], [1, 1]], dtype=np.uint8),
        )
        fractional.validate_label_values()

        label8 = dicom.seg.read_bytes(
            _make_labelmap_seg8().write_bytes_with_transfer_syntax(transfer_syntax)
        )
        np.testing.assert_array_equal(
            label8.to_array(0),
            np.array([[0, 1, 2], [2, 0, 1]], dtype=np.uint8),
        )
        np.testing.assert_array_equal(
            label8.to_array(1), np.array([[0, 0, 2], [0, 0, 0]], dtype=np.uint8)
        )
        assert label8.present_segment_numbers(0) == (1, 2)
        assert label8.present_segment_numbers(1) == (2,)
        np.testing.assert_array_equal(
            label8.mask_for_segment(0, 1),
            np.array([[0, 1, 0], [0, 0, 1]], dtype=np.uint8),
        )
        assert len(label8.frames_for_segment(2)) == 2
        assert len(label8.frames_for_segment(7)) == 0
        label8.validate_label_values()

        label16 = dicom.seg.read_bytes(
            _make_labelmap_seg16().write_bytes_with_transfer_syntax(transfer_syntax)
        )
        np.testing.assert_array_equal(
            label16.to_array(0), np.array([[0, 1], [300, 300]], dtype=np.uint16)
        )
        assert label16.present_segment_numbers(0) == (1, 300)
        np.testing.assert_array_equal(
            label16.mask_for_segment(0, 300),
            np.array([[0, 0], [1, 1]], dtype=np.uint8),
        )
        label16.validate_label_values()


def test_labelmap_sop_class_conflict_and_metadata_validation() -> None:
    conflict = _make_labelmap_seg8()
    seg_uid = dicom.uid_from_keyword("SegmentationStorage").value
    _set(conflict, "SOPClassUID", seg_uid)
    assert not dicom.seg.is_segmentation_storage(conflict)
    assert not dicom.seg.is_labelmap_segmentation_storage(conflict)
    assert not dicom.seg.is_any_segmentation_storage(conflict)
    with pytest.raises(Exception, match="disagree"):
        _seg_from_synthetic(conflict)

    palette = _make_labelmap_seg8()
    _set(palette, "PhotometricInterpretation", "PALETTE COLOR")
    np.testing.assert_array_equal(
        _seg_from_synthetic(palette).to_array(0),
        np.array([[0, 1, 2], [2, 0, 1]], dtype=np.uint8),
    )

    bad_photo = _make_labelmap_seg8()
    _set(bad_photo, "PhotometricInterpretation", "RGB")
    with pytest.raises(Exception, match="PhotometricInterpretation"):
        _seg_from_synthetic(bad_photo)

    pixel_padding_range = _make_labelmap_seg8()
    _set(pixel_padding_range, "PixelPaddingRangeLimit", 1)
    with pytest.raises(Exception, match="PixelPaddingRangeLimit"):
        _seg_from_synthetic(pixel_padding_range)

    missing_padding_segment = _make_labelmap_seg8()
    _set(missing_padding_segment, "PixelPaddingValue", 255)
    with pytest.raises(Exception, match="PixelPaddingValue.*SegmentNumber"):
        _seg_from_synthetic(missing_padding_segment)

    binary_zero_segment = _make_binary_seg()
    _set(binary_zero_segment, "SegmentSequence.0.SegmentNumber", 0)
    with pytest.raises(Exception, match="SegmentNumber 0"):
        _seg_from_synthetic(binary_zero_segment)


def test_labelmap_pixeldata_payload_policy_is_lazy_and_strict() -> None:
    missing_pixel_data = _make_labelmap_seg8()
    missing_pixel_data.remove_dataelement("PixelData")
    seg = _seg_from_synthetic(missing_pixel_data)
    with pytest.raises(Exception, match="PixelData is missing"):
        seg.decode_frame(0)

    legal_padding = _seg_from_synthetic(_make_labelmap_seg8_odd_frame(b"\x00\x01\x02\x00"))
    np.testing.assert_array_equal(
        legal_padding.to_array(),
        np.array([[0, 1, 2]], dtype=np.uint8),
    )

    bad_padding = _seg_from_synthetic(_make_labelmap_seg8_odd_frame(b"\x00\x01\x02\x03"))
    with pytest.raises(Exception, match="PixelData size mismatch"):
        bad_padding.decode_frame()

    trailing_data = _seg_from_synthetic(_make_labelmap_seg8(bytes(range(13))))
    with pytest.raises(Exception, match="PixelData size mismatch"):
        trailing_data.decode_frame(0)

    explicit_uid = dicom.uid_from_keyword("ExplicitVRLittleEndian").value.encode("ascii")
    rle_uid = dicom.uid_from_keyword("RLELossless").value.encode("ascii")
    compressed_bytes = bytearray(_make_labelmap_seg8().write_bytes())
    assert compressed_bytes.count(explicit_uid) >= 1
    compressed_bytes = compressed_bytes.replace(explicit_uid, rle_uid, 1)
    compressed = dicom.seg.read_bytes(compressed_bytes)
    with pytest.raises(Exception, match="compressed/encapsulated Label Map SEG"):
        compressed.decode_frame(0)

    big_endian_bytes = _make_labelmap_seg8().write_bytes_with_transfer_syntax(
        "ExplicitVRBigEndian"
    )
    big_endian = dicom.seg.read_bytes(big_endian_bytes)
    with pytest.raises(Exception, match="Big Endian LABELMAP"):
        big_endian.decode_frame(0)


def test_labelmap_metadata_only_absent_segment_and_exact_buffer_contract() -> None:
    df = _make_labelmap_seg8()
    _populate_segment(df, 4, 300, "Metadata Only")
    seg = _seg_from_synthetic(df)

    assert seg.segment_count == 5
    assert seg.present_segment_numbers(0) == (1, 2)
    np.testing.assert_array_equal(
        seg.mask_for_segment(0, 300), np.zeros((2, 3), dtype=np.uint8)
    )
    assert len(seg.frames_for_segment(300)) == 0
    assert seg.segment_frame_count(300) == 0


def test_labelmap_unknown_pixel_label_is_lazy_validation_error() -> None:
    seg = _seg_from_synthetic(_make_labelmap_seg8(bytes([0, 1, 9, 0, 0, 0] * 2)))

    assert seg.segment_count == 4
    with pytest.raises(Exception, match="undefined segment number"):
        seg.present_segment_numbers(0)
    with pytest.raises(Exception, match="undefined segment number"):
        seg.decode_frame(0)
    with pytest.raises(Exception, match="undefined segment number"):
        seg.validate_label_values()


def test_labelmap_16bit_uses_uint16_decode_contract() -> None:
    seg = _seg_from_synthetic(_make_labelmap_seg16())

    assert seg.labelmap_bits_allocated == 16
    expected = np.array([[0, 1], [300, 300]], dtype=np.uint16)
    decoded = seg.to_array()
    assert decoded.dtype == np.uint16
    np.testing.assert_array_equal(decoded, expected)
    assert seg.decode_frame() == expected.tobytes()
    assert seg.present_segment_numbers() == (1, 300)

    out16 = np.empty((2, 2), dtype=np.uint16)
    returned = seg.decode_frame_into(0, out16)
    assert returned is out16
    np.testing.assert_array_equal(out16, expected)
    with pytest.raises(Exception, match="itemsize|uint16|sample size"):
        seg.decode_frame_into(0, np.empty((2, 2), dtype=np.uint8))
    with pytest.raises(Exception, match="uint16"):
        seg.decode_frame_into(0, np.empty((2, 2), dtype=np.int16))
    with pytest.raises(Exception, match="native-endian"):
        seg.decode_frame_into(0, np.empty((2, 2), dtype=">u2"))
    np.testing.assert_array_equal(
        seg.mask_for_segment(0, 300),
        np.array([[0, 0], [1, 1]], dtype=np.uint8),
    )


def _assert_real_seg_sample(path: Path) -> None:
    seg = dicom.seg.read_file(path)
    assert seg.is_valid
    assert seg.segment_count > 0
    assert seg.frame_count > 0
    assert seg.rows > 0
    assert seg.columns > 0
    assert seg.frame_of_reference_uid
    decoded = seg.to_array(0)
    assert decoded.shape == (seg.rows, seg.columns)
    if (
        seg.segmentation_type == dicom.seg.SegmentationType.labelmap
        and seg.labelmap_bits_allocated == 16
    ):
        assert decoded.dtype == np.uint16
    else:
        assert decoded.dtype == np.uint8

    present = seg.present_segment_numbers(0)
    assert isinstance(present, tuple)
    if present:
        mask = seg.mask_for_segment(0, present[0])
        assert mask.shape == (seg.rows, seg.columns)
        assert mask.dtype == np.uint8
        assert np.isin(mask, (0, 1)).all()
        assert len(seg.frames_for_segment(present[0])) > 0

    seg.validate_label_values()


def test_optional_local_seg_sample_regression() -> None:
    path = os.environ.get("DICOMSDL_SEG_SAMPLE_PATH")
    if not path:
        pytest.skip("set DICOMSDL_SEG_SAMPLE_PATH to enable local SEG sample regression")

    _assert_real_seg_sample(Path(path))


def test_optional_local_seg_sample_directory_regression() -> None:
    root = Path(
        os.environ.get(
            "DICOMSDL_SEG_SAMPLE_DIR", REPO_ROOT.parent / "sample" / "seg"
        )
    )
    if not root.exists():
        pytest.skip(f"SEG sample directory is not available: {root}")

    paths = sorted(root.glob("*.dcm"))
    if not paths:
        pytest.skip(f"SEG sample directory has no DICOM files: {root}")

    for path in paths:
        _assert_real_seg_sample(path)
