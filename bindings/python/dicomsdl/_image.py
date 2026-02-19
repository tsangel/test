from __future__ import annotations

from . import _dicomsdl


def _get_tag_upper_text(ds: _dicomsdl.DataSet, keyword: str) -> str | None:
    text = ds.get_dataelement(keyword).to_string_view()
    if text is None:
        return None
    text = text.strip()
    if not text:
        return None
    return text.upper()


def _resolve_window(
    ds: _dicomsdl.DataSet, window: tuple[float, float] | None, auto_window: bool
) -> tuple[float, float] | None:
    if window is not None:
        if len(window) != 2:
            raise ValueError("window must be a (center, width) pair")
        center = float(window[0])
        width = float(window[1])
        if width <= 0:
            raise ValueError("window width must be > 0")
        return center, width

    if not auto_window:
        return None

    center = ds.get_dataelement("WindowCenter").to_double()
    width = ds.get_dataelement("WindowWidth").to_double()
    if center is None or width is None or width <= 0:
        return None
    return center, width


def _normalize_mono_to_uint8(array, window: tuple[float, float] | None):
    try:
        import numpy as np
    except ImportError as exc:  # pragma: no cover - numpy is required for to_array anyway.
        raise ImportError("DataSet.to_image requires NumPy to be installed.") from exc

    if array.dtype == np.uint8 and window is None:
        return array

    work = array.astype(np.float32, copy=False)
    if window is not None:
        center, width = window
        low = center - 0.5 - (width - 1.0) / 2.0
        high = center - 0.5 + (width - 1.0) / 2.0
        if high <= low:
            return np.zeros(array.shape, dtype=np.uint8)
        scaled = (work - low) * (255.0 / (high - low))
    else:
        min_value = float(work.min())
        max_value = float(work.max())
        if max_value <= min_value:
            return np.zeros(array.shape, dtype=np.uint8)
        scaled = (work - min_value) * (255.0 / (max_value - min_value))

    # Guard against NaN/Inf from upstream scaled pixel values.
    scaled = np.nan_to_num(scaled, nan=0.0, posinf=255.0, neginf=0.0)
    return np.clip(scaled, 0.0, 255.0).astype(np.uint8)


def _normalize_color_to_uint8(array):
    try:
        import numpy as np
    except ImportError as exc:  # pragma: no cover - numpy is required for to_array anyway.
        raise ImportError("DataSet.to_image requires NumPy to be installed.") from exc

    if array.dtype == np.uint8:
        return array

    work = array.astype(np.float32, copy=False)
    min_value = work.min(axis=(0, 1), keepdims=True)
    max_value = work.max(axis=(0, 1), keepdims=True)
    value_range = max_value - min_value
    safe_range = np.where(value_range > 0.0, value_range, 1.0)
    scaled = (work - min_value) * (255.0 / safe_range)
    scaled = np.where(value_range > 0.0, scaled, 0.0)
    # Guard against NaN/Inf from upstream scaled pixel values.
    scaled = np.nan_to_num(scaled, nan=0.0, posinf=255.0, neginf=0.0)
    return np.clip(scaled, 0.0, 255.0).astype(np.uint8)


def _should_convert_ybr_to_rgb(photometric: str | None) -> bool:
    if photometric is None:
        return False
    # JPEG2000 YBR_RCT/YBR_ICT are commonly converted to RGB during decode.
    # Converting again here causes color distortion.
    if photometric in {"YBR_RCT", "YBR_ICT"}:
        return False
    return photometric in {
        "YBR_FULL",
        "YBR_FULL_422",
        "YBR_PARTIAL_420",
        "YBR_PARTIAL_422",
    }


def _dataset_to_image(
    self: _dicomsdl.DataSet,
    frame: int = 0,
    *,
    window: tuple[float, float] | None = None,
    auto_window: bool = True,
):
    if frame < 0:
        raise ValueError("frame must be >= 0")

    try:
        from PIL import Image
    except ImportError as exc:
        raise ImportError("DataSet.to_image requires Pillow. Install with 'pip install pillow'.") from exc

    array = self.to_array(frame=frame, scaled=True)
    if array.ndim == 2:
        display_window = _resolve_window(self, window, auto_window)
        mono = _normalize_mono_to_uint8(array, display_window)
        photometric = _get_tag_upper_text(self, "PhotometricInterpretation")
        if photometric == "MONOCHROME1":
            mono = 255 - mono
        return Image.fromarray(mono, mode="L")

    if array.ndim == 3:
        channels = int(array.shape[2])
        if channels not in (3, 4):
            raise ValueError(f"unsupported color shape for to_image: {array.shape}")

        photometric = _get_tag_upper_text(self, "PhotometricInterpretation")
        if photometric == "PALETTE COLOR":
            raise NotImplementedError("to_image does not yet support PALETTE COLOR.")

        color = _normalize_color_to_uint8(array)
        if _should_convert_ybr_to_rgb(photometric):
            if channels != 3:
                raise ValueError("YBR photometric interpretation requires 3 samples per pixel")
            return Image.fromarray(color, mode="YCbCr").convert("RGB")

        mode = "RGB" if channels == 3 else "RGBA"
        return Image.fromarray(color, mode=mode)

    raise ValueError(f"unsupported pixel array ndim for to_image: {array.ndim}")


_dicomsdl.DataSet.to_image = _dataset_to_image
