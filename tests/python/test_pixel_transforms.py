from __future__ import annotations

import dicomsdl as dicom
import pytest

np = pytest.importorskip("numpy")


def _add_single_item_sequence(owner, keyword: str):
    sequence = owner.add_dataelement(keyword, dicom.VR.SQ).sequence
    assert sequence is not None
    item = sequence.add_dataset()
    assert item is not None
    return item


def test_apply_rescale_accepts_scalar_parameters():
    source = np.array([[1, 2], [3, 4]], dtype=np.int16)

    transformed = dicom.apply_rescale(source, 2.0, -1.0)

    assert isinstance(transformed, np.ndarray)
    assert transformed.dtype == np.float32
    assert np.allclose(transformed, source.astype(np.float32) * 2.0 - 1.0)


def test_apply_rescale_accepts_transform_object():
    source = np.array([[2, 4], [6, 8]], dtype=np.int16)
    transform = dicom.RescaleTransform()
    transform.slope = 0.5
    transform.intercept = 1.25

    transformed = dicom.apply_rescale(source, transform)

    assert transformed.dtype == np.float32
    assert np.allclose(transformed, source.astype(np.float32) * 0.5 + 1.25)


def test_apply_window_accepts_scalar_parameters():
    source = np.array([[-1024, 0, 400]], dtype=np.int16)

    transformed = dicom.apply_window(source, 0.0, 400.0)

    assert transformed.dtype == np.uint8
    assert np.array_equal(transformed, np.array([[0, 127, 255]], dtype=np.uint8))


def test_apply_window_accepts_transform_object():
    source = np.array([[0.0, 50.0, 100.0]], dtype=np.float32)
    window = dicom.WindowTransform()
    window.center = 50.0
    window.width = 100.0
    window.function = dicom.VoiLutFunction.linear_exact

    transformed = dicom.apply_window(source, window)

    assert transformed.dtype == np.uint8
    assert transformed.shape == source.shape
    assert transformed[0, 0] == 0
    assert transformed[0, 2] == 255


def test_apply_rescale_frames_applies_per_frame_coefficients():
    source = np.array(
        [
            [[1, 2], [3, 4]],
            [[5, 6], [7, 8]],
        ],
        dtype=np.int16,
    )

    transformed = dicom.apply_rescale_frames(
        source,
        [1.0, 10.0],
        [0.0, -5.0],
    )

    expected = np.array(
        [
            [[1.0, 2.0], [3.0, 4.0]],
            [[45.0, 55.0], [65.0, 75.0]],
        ],
        dtype=np.float32,
    )

    assert transformed.dtype == np.float32
    assert np.allclose(transformed, expected)


def test_apply_rescale_frames_rejects_ambiguous_3d_shape_with_trailing_3():
    source = np.zeros((2, 512, 3), dtype=np.int16)

    with pytest.raises(ValueError, match="ambiguous"):
        dicom.apply_rescale_frames(source, [1.0, 2.0], [0.0, 0.0])


def test_apply_rescale_frames_accepts_explicit_4d_singleton_channel_shape():
    source = np.array(
        [
            [[[1], [2]], [[3], [4]]],
            [[[5], [6]], [[7], [8]]],
        ],
        dtype=np.int16,
    )

    transformed = dicom.apply_rescale_frames(
        source,
        [1.0, 10.0],
        [0.0, -5.0],
    )

    expected = np.array(
        [
            [[1.0, 2.0], [3.0, 4.0]],
            [[45.0, 55.0], [65.0, 75.0]],
        ],
        dtype=np.float32,
    )

    assert transformed.dtype == np.float32
    assert np.allclose(transformed, expected)


def test_apply_modality_lut_maps_and_clamps_values():
    source = np.array([[9, 10], [11, 15]], dtype=np.int16)
    lut = dicom.ModalityLut()
    lut.first_mapped = 10
    lut.values = [100.0, 200.0, 300.0]

    transformed = dicom.apply_modality_lut(source, lut)

    expected = np.array([[100.0, 100.0], [200.0, 300.0]], dtype=np.float32)

    assert transformed.dtype == np.float32
    assert np.allclose(transformed, expected)


def test_apply_voi_lut_maps_and_clamps_values():
    source = np.array([[0, 1], [2, 9]], dtype=np.int16)
    lut = dicom.VoiLut()
    lut.first_mapped = 1
    lut.bits_per_entry = 8
    lut.values = [5, 10, 15]

    transformed = dicom.apply_voi_lut(source, lut)

    expected = np.array([[5, 5], [10, 15]], dtype=np.uint8)

    assert transformed.dtype == np.uint8
    assert np.array_equal(transformed, expected)


def test_apply_palette_lut_expands_indexed_values_to_rgb():
    source = np.array([[0, 1], [2, 3]], dtype=np.uint8)
    lut = dicom.PaletteLut()
    lut.first_mapped = 0
    lut.bits_per_entry = 8
    lut.red_values = [10, 20, 30, 40]
    lut.green_values = [1, 2, 3, 4]
    lut.blue_values = [100, 110, 120, 130]

    transformed = dicom.apply_palette_lut(source, lut)

    expected = np.array(
        [
            [[10, 1, 100], [20, 2, 110]],
            [[30, 3, 120], [40, 4, 130]],
        ],
        dtype=np.uint8,
    )

    assert transformed.dtype == np.uint8
    assert np.array_equal(transformed, expected)


def test_apply_palette_lut_expands_indexed_values_to_rgba_when_alpha_present():
    source = np.array([[0, 1], [2, 3]], dtype=np.uint8)
    lut = dicom.PaletteLut()
    lut.first_mapped = 0
    lut.bits_per_entry = 8
    lut.red_values = [10, 20, 30, 40]
    lut.green_values = [1, 2, 3, 4]
    lut.blue_values = [100, 110, 120, 130]
    lut.alpha_values = [200, 210, 220, 230]

    transformed = dicom.apply_palette_lut(source, lut)

    expected = np.array(
        [
            [[10, 1, 100, 200], [20, 2, 110, 210]],
            [[30, 3, 120, 220], [40, 4, 130, 230]],
        ],
        dtype=np.uint8,
    )

    assert transformed.dtype == np.uint8
    assert np.array_equal(transformed, expected)


def test_dicomfile_rescale_transform_property_reads_dataset_values():
    dicom_file = dicom.DicomFile()

    assert dicom_file.rescale_transform is None

    dicom_file.dataset.set_value("RescaleSlope", "2.5")
    dicom_file.dataset.set_value("RescaleIntercept", "-3.0")

    transform = dicom_file.rescale_transform

    assert isinstance(transform, dicom.RescaleTransform)
    assert transform.slope == pytest.approx(2.5)
    assert transform.intercept == pytest.approx(-3.0)


def test_dicomfile_rescale_transform_for_frame_prefers_per_frame_then_shared_then_root():
    dicom_file = dicom.DicomFile()
    dicom_file.dataset.set_value("NumberOfFrames", "2")
    dicom_file.dataset.set_value("RescaleSlope", "10")
    dicom_file.dataset.set_value("RescaleIntercept", "100")

    shared_fg = _add_single_item_sequence(dicom_file, "SharedFunctionalGroupsSequence")
    shared_tx = _add_single_item_sequence(shared_fg, "PixelValueTransformationSequence")
    shared_tx.set_value("RescaleSlope", "2")
    shared_tx.set_value("RescaleIntercept", "20")

    per_frame_sequence = dicom_file.add_dataelement(
        "PerFrameFunctionalGroupsSequence", dicom.VR.SQ
    ).sequence
    assert per_frame_sequence is not None
    frame0_fg = per_frame_sequence.add_dataset()
    frame1_fg = per_frame_sequence.add_dataset()
    assert frame0_fg is not None
    assert frame1_fg is not None
    frame0_tx = _add_single_item_sequence(frame0_fg, "PixelValueTransformationSequence")
    frame0_tx.set_value("RescaleSlope", "3")
    frame0_tx.set_value("RescaleIntercept", "30")

    frame0 = dicom_file.rescale_transform_for_frame(0)
    frame1 = dicom_file.rescale_transform_for_frame(1)
    default_frame = dicom_file.rescale_transform

    assert isinstance(frame0, dicom.RescaleTransform)
    assert isinstance(frame1, dicom.RescaleTransform)
    assert isinstance(default_frame, dicom.RescaleTransform)
    assert frame0.slope == pytest.approx(3.0)
    assert frame0.intercept == pytest.approx(30.0)
    assert frame1.slope == pytest.approx(2.0)
    assert frame1.intercept == pytest.approx(20.0)
    assert default_frame.slope == pytest.approx(3.0)
    assert default_frame.intercept == pytest.approx(30.0)


def test_dicomfile_window_transform_property_reads_dataset_values():
    dicom_file = dicom.DicomFile()

    assert dicom_file.window_transform is None

    dicom_file.dataset.set_value("WindowCenter", "40")
    dicom_file.dataset.set_value("WindowWidth", "400")
    dicom_file.dataset.set_value("VOILUTFunction", "SIGMOID")

    transform = dicom_file.window_transform

    assert isinstance(transform, dicom.WindowTransform)
    assert transform.center == pytest.approx(40.0)
    assert transform.width == pytest.approx(400.0)
    assert transform.function is dicom.VoiLutFunction.sigmoid


def test_dicomfile_window_transform_for_frame_prefers_per_frame_then_shared_then_root():
    dicom_file = dicom.DicomFile()
    dicom_file.dataset.set_value("NumberOfFrames", "2")
    dicom_file.dataset.set_value("WindowCenter", "10")
    dicom_file.dataset.set_value("WindowWidth", "100")

    shared_fg = _add_single_item_sequence(dicom_file, "SharedFunctionalGroupsSequence")
    shared_voi = _add_single_item_sequence(shared_fg, "FrameVOILUTSequence")
    shared_voi.set_value("WindowCenter", "20")
    shared_voi.set_value("WindowWidth", "200")
    shared_voi.set_value("VOILUTFunction", "SIGMOID")

    per_frame_sequence = dicom_file.add_dataelement(
        "PerFrameFunctionalGroupsSequence", dicom.VR.SQ
    ).sequence
    assert per_frame_sequence is not None
    frame0_fg = per_frame_sequence.add_dataset()
    frame1_fg = per_frame_sequence.add_dataset()
    assert frame0_fg is not None
    assert frame1_fg is not None
    frame0_voi = _add_single_item_sequence(frame0_fg, "FrameVOILUTSequence")
    frame0_voi.set_value("WindowCenter", "30")
    frame0_voi.set_value("WindowWidth", "300")
    frame0_voi.set_value("VOILUTFunction", "LINEAR_EXACT")

    frame0 = dicom_file.window_transform_for_frame(0)
    frame1 = dicom_file.window_transform_for_frame(1)
    default_frame = dicom_file.window_transform

    assert isinstance(frame0, dicom.WindowTransform)
    assert isinstance(frame1, dicom.WindowTransform)
    assert isinstance(default_frame, dicom.WindowTransform)
    assert frame0.center == pytest.approx(30.0)
    assert frame0.width == pytest.approx(300.0)
    assert frame0.function is dicom.VoiLutFunction.linear_exact
    assert frame1.center == pytest.approx(20.0)
    assert frame1.width == pytest.approx(200.0)
    assert frame1.function is dicom.VoiLutFunction.sigmoid
    assert default_frame.center == pytest.approx(30.0)
    assert default_frame.width == pytest.approx(300.0)


def test_dicomfile_modality_lut_property_defaults_to_none():
    dicom_file = dicom.DicomFile()

    assert dicom_file.modality_lut is None


def test_dicomfile_modality_lut_for_frame_shares_root_sequence():
    dicom_file = dicom.DicomFile()
    dicom_file.dataset.set_value("NumberOfFrames", "2")

    modality_sequence = dicom_file.add_dataelement("ModalityLUTSequence", dicom.VR.SQ).sequence
    assert modality_sequence is not None
    item = modality_sequence.add_dataset()
    assert item is not None
    assert item.set_value("LUTDescriptor", dicom.VR.US, [3, 10, 8])
    item.add_dataelement("LUTData", dicom.VR.OW).value = bytes([11, 22, 33])

    frame0 = dicom_file.modality_lut_for_frame(0)
    frame1 = dicom_file.modality_lut_for_frame(1)
    default_frame = dicom_file.modality_lut

    assert isinstance(frame0, dicom.ModalityLut)
    assert isinstance(frame1, dicom.ModalityLut)
    assert isinstance(default_frame, dicom.ModalityLut)
    assert frame0.first_mapped == 10
    assert frame0.values == [11.0, 22.0, 33.0]
    assert frame1.values == [11.0, 22.0, 33.0]
    assert default_frame.values == [11.0, 22.0, 33.0]


def test_dicomfile_voi_lut_property_reads_dataset_values():
    dicom_file = dicom.DicomFile()

    assert dicom_file.voi_lut is None

    voi_sequence = dicom_file.add_dataelement("VOILUTSequence", dicom.VR.SQ).sequence
    assert voi_sequence is not None
    item = voi_sequence.add_dataset()
    assert item.set_value("LUTDescriptor", dicom.VR.US, [4, 0, 8])
    item.add_dataelement("LUTData", dicom.VR.OW).value = bytes([9, 8, 7, 6])

    lut = dicom_file.voi_lut

    assert isinstance(lut, dicom.VoiLut)
    assert lut.first_mapped == 0
    assert lut.bits_per_entry == 8
    assert lut.values == [9, 8, 7, 6]


def test_dicomfile_voi_lut_for_frame_prefers_per_frame_then_shared_then_root():
    dicom_file = dicom.DicomFile()
    dicom_file.dataset.set_value("NumberOfFrames", "2")

    root_voi = _add_single_item_sequence(dicom_file, "VOILUTSequence")
    assert root_voi.set_value("LUTDescriptor", dicom.VR.US, [2, 0, 8])
    root_voi.add_dataelement("LUTData", dicom.VR.OW).value = bytes([1, 2])

    shared_fg = _add_single_item_sequence(dicom_file, "SharedFunctionalGroupsSequence")
    shared_voi = _add_single_item_sequence(shared_fg, "FrameVOILUTSequence")
    shared_lut = _add_single_item_sequence(shared_voi, "VOILUTSequence")
    assert shared_lut.set_value("LUTDescriptor", dicom.VR.US, [2, 0, 8])
    shared_lut.add_dataelement("LUTData", dicom.VR.OW).value = bytes([3, 4])

    per_frame_sequence = dicom_file.add_dataelement(
        "PerFrameFunctionalGroupsSequence", dicom.VR.SQ
    ).sequence
    assert per_frame_sequence is not None
    frame0_fg = per_frame_sequence.add_dataset()
    frame1_fg = per_frame_sequence.add_dataset()
    assert frame0_fg is not None
    assert frame1_fg is not None
    frame0_voi = _add_single_item_sequence(frame0_fg, "FrameVOILUTSequence")
    frame0_lut = _add_single_item_sequence(frame0_voi, "VOILUTSequence")
    assert frame0_lut.set_value("LUTDescriptor", dicom.VR.US, [2, 0, 8])
    frame0_lut.add_dataelement("LUTData", dicom.VR.OW).value = bytes([5, 6])

    frame0 = dicom_file.voi_lut_for_frame(0)
    frame1 = dicom_file.voi_lut_for_frame(1)
    default_frame = dicom_file.voi_lut

    assert isinstance(frame0, dicom.VoiLut)
    assert isinstance(frame1, dicom.VoiLut)
    assert isinstance(default_frame, dicom.VoiLut)
    assert frame0.values == [5, 6]
    assert frame1.values == [3, 4]
    assert default_frame.values == [5, 6]


def test_dicomfile_palette_lut_property_reads_dataset_values():
    dicom_file = dicom.DicomFile()

    assert dicom_file.palette_lut is None

    descriptor = [4, 0, 8]
    dicom_file.dataset.set_value("RedPaletteColorLookupTableDescriptor", dicom.VR.US, descriptor)
    dicom_file.dataset.set_value("GreenPaletteColorLookupTableDescriptor", dicom.VR.US, descriptor)
    dicom_file.dataset.set_value("BluePaletteColorLookupTableDescriptor", dicom.VR.US, descriptor)
    dicom_file.dataset.set_value("AlphaPaletteColorLookupTableDescriptor", dicom.VR.US, descriptor)
    dicom_file.add_dataelement("RedPaletteColorLookupTableData", dicom.VR.OW).value = bytes(
        [10, 20, 30, 40]
    )
    dicom_file.add_dataelement("GreenPaletteColorLookupTableData", dicom.VR.OW).value = bytes(
        [1, 2, 3, 4]
    )
    dicom_file.add_dataelement("BluePaletteColorLookupTableData", dicom.VR.OW).value = bytes(
        [100, 110, 120, 130]
    )
    dicom_file.add_dataelement("AlphaPaletteColorLookupTableData", dicom.VR.OW).value = bytes(
        [255, 200, 150, 100]
    )

    lut = dicom_file.palette_lut

    assert isinstance(lut, dicom.PaletteLut)
    assert lut.first_mapped == 0
    assert lut.bits_per_entry == 8
    assert lut.red_values == [10, 20, 30, 40]
    assert lut.green_values == [1, 2, 3, 4]
    assert lut.blue_values == [100, 110, 120, 130]


def test_dicomfile_palette_lut_property_expands_segmented_palette_data():
    dicom_file = dicom.DicomFile()

    descriptor = [8, 0, 8]
    dicom_file.dataset.set_value("RedPaletteColorLookupTableDescriptor", dicom.VR.US, descriptor)
    dicom_file.dataset.set_value("GreenPaletteColorLookupTableDescriptor", dicom.VR.US, descriptor)
    dicom_file.dataset.set_value("BluePaletteColorLookupTableDescriptor", dicom.VR.US, descriptor)
    dicom_file.add_dataelement(
        "SegmentedRedPaletteColorLookupTableData", dicom.VR.OW
    ).value = bytes([0, 1, 10, 1, 3, 40, 2, 2, 0, 0, 0, 0])
    dicom_file.add_dataelement(
        "SegmentedGreenPaletteColorLookupTableData", dicom.VR.OW
    ).value = bytes([0, 1, 1, 1, 3, 4, 2, 2, 0, 0, 0, 0])
    dicom_file.add_dataelement(
        "SegmentedBluePaletteColorLookupTableData", dicom.VR.OW
    ).value = bytes([0, 1, 100, 1, 3, 130, 2, 2, 0, 0, 0, 0])

    lut = dicom_file.palette_lut

    assert isinstance(lut, dicom.PaletteLut)
    assert lut.first_mapped == 0
    assert lut.bits_per_entry == 8
    assert lut.red_values == [10, 20, 30, 40, 10, 20, 30, 40]
    assert lut.green_values == [1, 2, 3, 4, 1, 2, 3, 4]
    assert lut.blue_values == [100, 110, 120, 130, 100, 110, 120, 130]


def test_dicomfile_supplemental_palette_property_is_separate_from_classic_palette_lut():
    dicom_file = dicom.DicomFile()

    dicom_file.PhotometricInterpretation = "MONOCHROME2"
    dicom_file.PixelPresentation = "COLOR"
    descriptor = [4, 0, 8]
    dicom_file.dataset.set_value("RedPaletteColorLookupTableDescriptor", dicom.VR.US, descriptor)
    dicom_file.dataset.set_value("GreenPaletteColorLookupTableDescriptor", dicom.VR.US, descriptor)
    dicom_file.dataset.set_value("BluePaletteColorLookupTableDescriptor", dicom.VR.US, descriptor)
    dicom_file.dataset.set_value("AlphaPaletteColorLookupTableDescriptor", dicom.VR.US, descriptor)
    dicom_file.add_dataelement("RedPaletteColorLookupTableData", dicom.VR.OW).value = bytes(
        [10, 20, 30, 40]
    )
    dicom_file.add_dataelement("GreenPaletteColorLookupTableData", dicom.VR.OW).value = bytes(
        [1, 2, 3, 4]
    )
    dicom_file.add_dataelement("BluePaletteColorLookupTableData", dicom.VR.OW).value = bytes(
        [100, 110, 120, 130]
    )
    dicom_file.add_dataelement("AlphaPaletteColorLookupTableData", dicom.VR.OW).value = bytes(
        [255, 200, 150, 100]
    )
    stored_value_range = _add_single_item_sequence(dicom_file, "StoredValueColorRangeSequence")
    assert stored_value_range.set_value("MinimumStoredValueMapped", dicom.VR.FD, 10.0)
    assert stored_value_range.set_value("MaximumStoredValueMapped", dicom.VR.FD, 4095.0)

    assert dicom_file.palette_lut is None
    supplemental = dicom_file.supplemental_palette
    assert isinstance(supplemental, dicom.SupplementalPaletteInfo)
    assert supplemental.pixel_presentation is dicom.PixelPresentation.color
    assert supplemental.has_stored_value_range
    assert supplemental.palette.red_values == [10, 20, 30, 40]
    assert supplemental.palette.blue_values == [100, 110, 120, 130]
    assert supplemental.palette.alpha_values == [255, 200, 150, 100]


def test_dicomfile_enhanced_palette_property_reads_pipeline_metadata():
    dicom_file = dicom.DicomFile()

    dicom_file.PixelPresentation = "COLOR"
    assignment = _add_single_item_sequence(dicom_file, "DataFrameAssignmentSequence")
    assert assignment.set_value("DataType", dicom.VR.CS, "TISSUE")
    assert assignment.set_value("DataPathAssignment", dicom.VR.CS, "PRIMARY_SINGLE")
    assert assignment.set_value("BitsMappedToColorLookupTable", dicom.VR.US, 12)

    blending = _add_single_item_sequence(dicom_file, "BlendingLUT1Sequence")
    assert blending.set_value("BlendingLUT1TransferFunction", dicom.VR.CS, "CONSTANT")
    assert blending.set_value("BlendingWeightConstant", dicom.VR.FD, 0.75)

    palette_item = _add_single_item_sequence(
        dicom_file, "EnhancedPaletteColorLookupTableSequence"
    )
    assert palette_item.set_value("DataPathID", dicom.VR.CS, "PRIMARY")
    assert palette_item.set_value("RGBLUTTransferFunction", dicom.VR.CS, "IDENTITY")
    descriptor = [2, 0, 8]
    assert palette_item.set_value("RedPaletteColorLookupTableDescriptor", dicom.VR.US, descriptor)
    assert palette_item.set_value(
        "GreenPaletteColorLookupTableDescriptor", dicom.VR.US, descriptor
    )
    assert palette_item.set_value("BluePaletteColorLookupTableDescriptor", dicom.VR.US, descriptor)
    assert palette_item.set_value(
        "AlphaPaletteColorLookupTableDescriptor", dicom.VR.US, descriptor
    )
    palette_item.add_dataelement("RedPaletteColorLookupTableData", dicom.VR.OW).value = bytes(
        [9, 19]
    )
    palette_item.add_dataelement("GreenPaletteColorLookupTableData", dicom.VR.OW).value = bytes(
        [29, 39]
    )
    palette_item.add_dataelement("BluePaletteColorLookupTableData", dicom.VR.OW).value = bytes(
        [49, 59]
    )
    palette_item.add_dataelement("AlphaPaletteColorLookupTableData", dicom.VR.OW).value = bytes(
        [69, 79]
    )
    dicom_file.ColorSpace = "SRGB"

    assert dicom_file.palette_lut is None
    enhanced = dicom_file.enhanced_palette
    assert isinstance(enhanced, dicom.EnhancedPaletteInfo)
    assert enhanced.pixel_presentation is dicom.PixelPresentation.color
    assert len(enhanced.data_frame_assignments) == 1
    assert enhanced.data_frame_assignments[0].data_path_assignment == "PRIMARY_SINGLE"
    assert enhanced.has_blending_lut_1
    assert abs(enhanced.blending_lut_1.weight_constant - 0.75) < 1e-6
    assert len(enhanced.palette_items) == 1
    assert enhanced.palette_items[0].data_path_id == "PRIMARY"
    assert enhanced.palette_items[0].palette.red_values == [9, 19]
    assert enhanced.palette_items[0].palette.alpha_values == [69, 79]
