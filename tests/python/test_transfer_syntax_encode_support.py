import dicomsdl as dicom
import pytest

np = pytest.importorskip("numpy")


def _encoder_options(keyword: str):
    if keyword in {"JPEGBaseline8Bit", "JPEGExtended12Bit"}:
        return {"type": "jpeg", "quality": 90}
    if keyword in {
        "JPEGLossless",
        "JPEGLosslessNonHierarchical15",
        "JPEGLosslessSV1",
    }:
        return {"type": "jpeg"}
    if keyword == "JPEGLSLossless":
        return {"type": "jpegls", "near_lossless_error": 0}
    if keyword == "JPEGLSNearLossless":
        return {"type": "jpegls", "near_lossless_error": 1}
    if keyword in {"JPEG2000", "JPEG2000MC"}:
        return {"type": "j2k", "target_psnr": 45}
    if keyword in {"JPEG2000Lossless", "JPEG2000MCLossless"}:
        return {"type": "j2k"}
    if keyword == "HTJ2K":
        return {"type": "htj2k", "target_psnr": 50}
    if keyword in {"HTJ2KLossless", "HTJ2KLosslessRPCL"}:
        return {"type": "htj2k"}
    if keyword == "JPEGXL":
        return {"type": "jpegxl", "distance": 1.0, "effort": 7}
    if keyword == "JPEGXLLossless":
        return {"type": "jpegxl", "distance": 0.0}
    if keyword == "RLELossless":
        return {"type": "rle"}
    return None


def test_transfer_syntax_uids_encode_supported_are_usable():
    source = np.arange(12, dtype=np.uint8).reshape(2, 2, 3)
    supported = list(dicom.transfer_syntax_uids_encode_supported())
    keywords = [uid.keyword or uid.value for uid in supported]

    assert supported
    assert "RLELossless" in keywords

    for uid in supported:
        keyword = uid.keyword or uid.value
        df = dicom.DicomFile()
        options = _encoder_options(keyword)
        try:
            if options is None:
                df.set_pixel_data(keyword, source)
            else:
                df.set_pixel_data(keyword, source, options=options)
        except Exception as exc:  # pragma: no cover - we want the exact failure surfaced
            raise AssertionError(
                f"transfer_syntax_uids_encode_supported returned unusable transfer syntax: {keyword}: {exc}"
            ) from exc
