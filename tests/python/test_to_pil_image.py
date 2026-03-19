from pathlib import Path

import dicomsdl as dicom
import pytest

np = pytest.importorskip("numpy")
ImageModule = pytest.importorskip("PIL.Image")


def _test_file(name: str = "test_le.dcm") -> str:
    return str(Path(__file__).resolve().parent.parent / name)


def test_to_pil_image_method_exists():
    assert hasattr(dicom.DicomFile, "to_pil_image")
    assert not hasattr(dicom.DataSet, "to_pil_image")


def test_to_pil_image_single_frame_default():
    dicom_file = dicom.read_file(_test_file())
    image = dicom_file.to_pil_image(frame=0)

    assert isinstance(image, ImageModule.Image)
    assert image.mode == "L"
    assert image.size == (4, 4)

    arr = np.asarray(image)
    assert arr.shape == (4, 4)
    assert arr.dtype == np.uint8


def test_to_pil_image_frame_validation():
    dicom_file = dicom.read_file(_test_file())

    with pytest.raises(ValueError):
        dicom_file.to_pil_image(frame=-1)

    with pytest.raises(IndexError):
        dicom_file.to_pil_image(frame=1)


def test_to_pil_image_explicit_window():
    dicom_file = dicom.read_file(_test_file())
    image = dicom_file.to_pil_image(window=(0.0, 400.0))

    arr = np.asarray(image)
    assert arr.shape == (4, 4)
    assert arr.dtype == np.uint8


def test_to_pil_image_uses_voi_lut_when_no_window_metadata():
    dicom_file = dicom.read_file(_test_file())
    dicom_file.remove_dataelement("WindowCenter")
    dicom_file.remove_dataelement("WindowWidth")
    dicom_file.remove_dataelement("VOILUTFunction")

    source = dicom_file.to_array(frame=0)
    first_mapped = int(source.min())
    last_mapped = int(source.max())
    entry_count = last_mapped - first_mapped + 1
    bits_per_entry = 8 if entry_count <= 256 else 16
    values = [entry_count - 1 - index for index in range(entry_count)]

    voi_sequence = dicom_file.add_dataelement("VOILUTSequence", dicom.VR.SQ).sequence
    assert voi_sequence is not None
    item = voi_sequence.add_dataset()
    assert item.set_value("LUTDescriptor", dicom.VR.US, [entry_count, first_mapped, bits_per_entry])
    if bits_per_entry <= 8:
        item.add_dataelement("LUTData", dicom.VR.OW).value = bytes(values)
    else:
        item.add_dataelement("LUTData", dicom.VR.OW).value = np.asarray(
            values, dtype=np.uint16
        ).tobytes()

    image = dicom_file.to_pil_image(frame=0, auto_window=True)

    lut = dicom_file.voi_lut
    assert isinstance(lut, dicom.VoiLut)
    expected = dicom.apply_voi_lut(source, lut)
    if expected.dtype != np.uint8:
        scale = 255.0 / float((1 << int(lut.bits_per_entry)) - 1)
        expected = np.clip(expected.astype(np.float32) * scale, 0.0, 255.0).astype(np.uint8)

    arr = np.asarray(image)
    assert arr.dtype == np.uint8
    assert np.array_equal(arr, expected)


def test_to_pil_image_supports_classic_palette_color():
    dicom_file = dicom.read_file(_test_file())
    source = np.array([[0, 1], [2, 3]], dtype=np.uint8)
    dicom_file.set_pixel_data("ExplicitVRLittleEndian", source)
    dicom_file.PhotometricInterpretation = "PALETTE COLOR"

    descriptor = [4, 0, 8]
    dicom_file.dataset.set_value("RedPaletteColorLookupTableDescriptor", dicom.VR.US, descriptor)
    dicom_file.dataset.set_value(
        "GreenPaletteColorLookupTableDescriptor", dicom.VR.US, descriptor
    )
    dicom_file.dataset.set_value("BluePaletteColorLookupTableDescriptor", dicom.VR.US, descriptor)
    dicom_file.add_dataelement("RedPaletteColorLookupTableData", dicom.VR.OW).value = bytes(
        [10, 20, 30, 40]
    )
    dicom_file.add_dataelement("GreenPaletteColorLookupTableData", dicom.VR.OW).value = bytes(
        [1, 2, 3, 4]
    )
    dicom_file.add_dataelement("BluePaletteColorLookupTableData", dicom.VR.OW).value = bytes(
        [100, 110, 120, 130]
    )

    image = dicom_file.to_pil_image(frame=0, auto_window=True)

    expected = dicom.apply_palette_lut(source, dicom_file.palette_lut)
    arr = np.asarray(image)
    assert image.mode == "RGB"
    assert arr.dtype == np.uint8
    assert np.array_equal(arr, expected)


def test_to_pil_image_supports_supplemental_palette_display():
    dicom_file = dicom.read_file(_test_file())
    source = np.array([[0, 1], [2, 3]], dtype=np.uint8)
    dicom_file.set_pixel_data("ExplicitVRLittleEndian", source)
    dicom_file.PhotometricInterpretation = "MONOCHROME2"
    dicom_file.PixelPresentation = "COLOR"

    descriptor = [4, 0, 8]
    dicom_file.dataset.set_value("RedPaletteColorLookupTableDescriptor", dicom.VR.US, descriptor)
    dicom_file.dataset.set_value(
        "GreenPaletteColorLookupTableDescriptor", dicom.VR.US, descriptor
    )
    dicom_file.dataset.set_value("BluePaletteColorLookupTableDescriptor", dicom.VR.US, descriptor)
    dicom_file.add_dataelement("RedPaletteColorLookupTableData", dicom.VR.OW).value = bytes(
        [10, 20, 30, 40]
    )
    dicom_file.add_dataelement("GreenPaletteColorLookupTableData", dicom.VR.OW).value = bytes(
        [1, 2, 3, 4]
    )
    dicom_file.add_dataelement("BluePaletteColorLookupTableData", dicom.VR.OW).value = bytes(
        [100, 110, 120, 130]
    )

    image = dicom_file.to_pil_image(frame=0, auto_window=True)

    supplemental = dicom_file.supplemental_palette
    expected = dicom.apply_palette_lut(source, supplemental.palette)
    arr = np.asarray(image)
    assert image.mode == "RGB"
    assert arr.dtype == np.uint8
    assert np.array_equal(arr, expected)


def test_to_pil_image_supports_simple_enhanced_palette_pipeline():
    dicom_file = dicom.DicomFile()
    source = np.array([[0, 255], [255, 0]], dtype=np.uint8)
    dicom_file.set_pixel_data("ExplicitVRLittleEndian", source)
    dicom_file.PhotometricInterpretation = "MONOCHROME2"
    dicom_file.PixelPresentation = "COLOR"

    assignment = dicom_file.add_dataelement("DataFrameAssignmentSequence", dicom.VR.SQ).sequence
    assert assignment is not None
    assignment_item = assignment.add_dataset()
    assert assignment_item.set_value("DataType", dicom.VR.CS, "TISSUE")
    assert assignment_item.set_value("DataPathAssignment", dicom.VR.CS, "PRIMARY_SINGLE")
    assert assignment_item.set_value("BitsMappedToColorLookupTable", dicom.VR.US, 1)

    palette_sequence = dicom_file.add_dataelement(
        "EnhancedPaletteColorLookupTableSequence", dicom.VR.SQ
    ).sequence
    assert palette_sequence is not None
    palette_item = palette_sequence.add_dataset()
    assert palette_item.set_value("DataPathID", dicom.VR.CS, "PRIMARY")
    assert palette_item.set_value("RGBLUTTransferFunction", dicom.VR.CS, "IDENTITY")
    assert palette_item.set_value("AlphaLUTTransferFunction", dicom.VR.CS, "IDENTITY")
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
        [10, 20]
    )
    palette_item.add_dataelement("GreenPaletteColorLookupTableData", dicom.VR.OW).value = bytes(
        [30, 40]
    )
    palette_item.add_dataelement("BluePaletteColorLookupTableData", dicom.VR.OW).value = bytes(
        [50, 60]
    )
    palette_item.add_dataelement("AlphaPaletteColorLookupTableData", dicom.VR.OW).value = bytes(
        [70, 80]
    )
    dicom_file.ColorSpace = "SRGB"

    image = dicom_file.to_pil_image(frame=0, auto_window=True)

    arr = np.asarray(image)
    expected = np.array(
        [
            [[10, 30, 50, 70], [20, 40, 60, 80]],
            [[20, 40, 60, 80], [10, 30, 50, 70]],
        ],
        dtype=np.uint8,
    )
    assert image.mode == "RGBA"
    assert arr.dtype == np.uint8
    assert np.array_equal(arr, expected)


def test_to_pil_image_rejects_complex_enhanced_palette_pipeline():
    dicom_file = dicom.read_file(_test_file())
    source = np.array([[0, 1], [0, 1]], dtype=np.uint8)
    dicom_file.set_pixel_data("ExplicitVRLittleEndian", source)
    dicom_file.PixelPresentation = "COLOR"

    assignment = dicom_file.add_dataelement("DataFrameAssignmentSequence", dicom.VR.SQ).sequence
    assert assignment is not None
    assignment_item = assignment.add_dataset()
    assert assignment_item.set_value("DataType", dicom.VR.CS, "TISSUE")
    assert assignment_item.set_value("DataPathAssignment", dicom.VR.CS, "PRIMARY_SINGLE")

    palette_sequence = dicom_file.add_dataelement(
        "EnhancedPaletteColorLookupTableSequence", dicom.VR.SQ
    ).sequence
    assert palette_sequence is not None
    palette_item = palette_sequence.add_dataset()
    assert palette_item.set_value("DataPathID", dicom.VR.CS, "PRIMARY")
    assert palette_item.set_value("RGBLUTTransferFunction", dicom.VR.CS, "IDENTITY")
    descriptor = [2, 0, 8]
    assert palette_item.set_value("RedPaletteColorLookupTableDescriptor", dicom.VR.US, descriptor)
    assert palette_item.set_value(
        "GreenPaletteColorLookupTableDescriptor", dicom.VR.US, descriptor
    )
    assert palette_item.set_value("BluePaletteColorLookupTableDescriptor", dicom.VR.US, descriptor)
    palette_item.add_dataelement("RedPaletteColorLookupTableData", dicom.VR.OW).value = bytes(
        [10, 20]
    )
    palette_item.add_dataelement("GreenPaletteColorLookupTableData", dicom.VR.OW).value = bytes(
        [30, 40]
    )
    palette_item.add_dataelement("BluePaletteColorLookupTableData", dicom.VR.OW).value = bytes(
        [50, 60]
    )

    with pytest.raises(NotImplementedError, match="Enhanced Palette"):
        dicom_file.to_pil_image(frame=0, auto_window=True)
