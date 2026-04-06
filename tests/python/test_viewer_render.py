import dicomsdl as dicom
import pytest

np = pytest.importorskip("numpy")
QtGui = pytest.importorskip("PySide6.QtGui")

from dicomsdl._viewer_render import render_loaded_dicom


def _supports_encode(keyword: str) -> bool:
    for uid in dicom.transfer_syntax_uids_encode_supported():
        if (uid.keyword or uid.value) == keyword:
            return True
    return False


def _qimage_to_rgb_array(image):
    image = image.convertToFormat(QtGui.QImage.Format.Format_RGB888)
    ptr = image.bits()
    size = image.sizeInBytes()
    try:
        ptr.setsize(size)
        buffer = ptr
    except AttributeError:
        buffer = ptr[:size]
    array = np.frombuffer(buffer, dtype=np.uint8)
    array = array.reshape(image.height(), image.bytesPerLine())
    return (
        array[:, : image.width() * 3].reshape(image.height(), image.width(), 3).copy()
    )


def test_render_loaded_dicom_uses_decoded_photometric_for_color_display():
    if not _supports_encode("JPEGBaseline8Bit"):
        pytest.skip("JPEGBaseline8Bit encoder is not available in this build")

    app = QtGui.QGuiApplication.instance() or QtGui.QGuiApplication([])

    dicom_file = dicom.DicomFile()
    source = np.array(
        [
            [[255, 0, 0], [0, 255, 0]],
            [[0, 0, 255], [255, 255, 0]],
        ],
        dtype=np.uint8,
    )
    dicom_file.set_pixel_data("JPEGBaseline8Bit", source)
    dicom_file.PhotometricInterpretation = "YBR_FULL_422"

    decoded, info = dicom_file.to_array(frame=0, with_info=True)
    rendered = render_loaded_dicom(dicom_file)
    displayed = _qimage_to_rgb_array(rendered.pixmap.toImage())

    assert app is not None
    assert info.photometric == dicom.Photometric.rgb
    assert rendered.photometric == "YBR_FULL_422"
    assert np.array_equal(displayed, decoded)
