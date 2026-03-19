from __future__ import annotations

from . import _dicomsdl


def _get_tag_int(ds: _dicomsdl.DataSet, keyword: str) -> int | None:
    value = ds.get_dataelement(keyword).to_long()
    if value is None:
        return None
    return int(value)


def _get_tag_upper_text(ds: _dicomsdl.DataSet, keyword: str) -> str | None:
    text = ds.get_dataelement(keyword).to_string_view()
    if text is None:
        return None
    text = text.strip()
    if not text:
        return None
    return text.upper()


def _resolve_window_transform(
    dicom_file: _dicomsdl.DicomFile,
    frame: int,
    window: tuple[float, float] | None,
    auto_window: bool,
) -> _dicomsdl.WindowTransform | None:
    if window is not None:
        if len(window) != 2:
            raise ValueError("window must be a (center, width) pair")
        transform = _dicomsdl.WindowTransform()
        transform.center = float(window[0])
        transform.width = float(window[1])
        transform.function = _dicomsdl.VoiLutFunction.linear
        if transform.width <= 0:
            raise ValueError("window width must be > 0")
        return transform

    if not auto_window:
        return None

    return dicom_file.window_transform_for_frame(frame)


def _resolve_voi_lut(
    dicom_file: _dicomsdl.DicomFile,
    frame: int,
    window: tuple[float, float] | None,
    auto_window: bool,
) -> _dicomsdl.VoiLut | None:
    if window is not None or not auto_window:
        return None
    if dicom_file.window_transform_for_frame(frame) is not None:
        return None
    return dicom_file.voi_lut_for_frame(frame)


def _normalize_mono_to_uint8(
    array,
    window: _dicomsdl.WindowTransform | None,
    voi_lut: _dicomsdl.VoiLut | None = None,
):
    try:
        import numpy as np
    except ImportError as exc:  # pragma: no cover - numpy is required for to_array anyway.
        raise ImportError("DicomFile.to_pil_image requires NumPy to be installed.") from exc

    if array.dtype == np.uint8 and window is None and voi_lut is None:
        return array

    if window is not None:
        work = array.astype(np.float32, copy=False)
        return _dicomsdl.apply_window(work, window)
    elif voi_lut is not None:
        mapped = _dicomsdl.apply_voi_lut(array, voi_lut)
        if mapped.dtype == np.uint8:
            return mapped

        bits_per_entry = int(voi_lut.bits_per_entry)
        if bits_per_entry <= 0:
            max_value = int(mapped.max()) if mapped.size != 0 else 0
            bits_per_entry = 8 if max_value <= 0xFF else 16
        scale = 255.0 / float((1 << bits_per_entry) - 1)
        scaled = mapped.astype(np.float32, copy=False) * scale
    else:
        work = array.astype(np.float32, copy=False)
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
        raise ImportError("DicomFile.to_pil_image requires NumPy to be installed.") from exc

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


def _convert_ybr_to_rgb(array, photometric: str | None):
    try:
        import numpy as np
    except ImportError as exc:  # pragma: no cover - numpy is required for to_array anyway.
        raise ImportError("DicomFile.to_pil_image requires NumPy to be installed.") from exc

    if not _should_convert_ybr_to_rgb(photometric):
        return array

    if array.ndim != 3 or int(array.shape[2]) != 3:
        raise ValueError("YBR photometric interpretation requires 3 samples per pixel")

    work = array.astype(np.float32, copy=False)
    y = work[..., 0]
    cb = work[..., 1] - 128.0
    cr = work[..., 2] - 128.0

    rgb = np.empty_like(work, dtype=np.float32)
    if photometric in {"YBR_PARTIAL_420", "YBR_PARTIAL_422"}:
        # YBR_PARTIAL uses studio-range luma/chroma, so expand Y from [16, 235]
        # before converting to RGB.
        y = 1.16438356 * (y - 16.0)
        rgb[..., 0] = y + 1.59602678 * cr
        rgb[..., 1] = y - 0.39176229 * cb - 0.81296764 * cr
        rgb[..., 2] = y + 2.01723214 * cb
    else:
        rgb[..., 0] = y + 1.402 * cr
        rgb[..., 1] = y - 0.344136 * cb - 0.714136 * cr
        rgb[..., 2] = y + 1.772 * cb
    rgb = np.nan_to_num(rgb, nan=0.0, posinf=255.0, neginf=0.0)
    return np.clip(rgb, 0.0, 255.0).astype(np.uint8)


def _normalize_palette_to_uint8(array, bits_per_entry: int):
    try:
        import numpy as np
    except ImportError as exc:  # pragma: no cover - numpy is required for to_array anyway.
        raise ImportError("DicomFile.to_pil_image requires NumPy to be installed.") from exc

    if array.dtype == np.uint8 and bits_per_entry <= 8:
        return array

    if bits_per_entry <= 0:
        max_value = int(array.max()) if array.size != 0 else 0
        bits_per_entry = 8 if max_value <= 0xFF else 16

    max_value = float((1 << bits_per_entry) - 1)
    if max_value <= 0.0:
        return np.zeros(array.shape, dtype=np.uint8)

    scaled = array.astype(np.float32, copy=False) * (255.0 / max_value)
    scaled = np.nan_to_num(scaled, nan=0.0, posinf=255.0, neginf=0.0)
    return np.clip(scaled, 0.0, 255.0).astype(np.uint8)


def _resolve_display_palette(
    dicom_file: _dicomsdl.DicomFile,
    photometric: str | None,
):
    enhanced = dicom_file.enhanced_palette
    if enhanced is not None:
        return ("enhanced", enhanced)

    if photometric == "PALETTE COLOR":
        palette = dicom_file.palette_lut
        if palette is None:
            raise ValueError("PALETTE COLOR image is missing classic palette LUT metadata.")
        return ("classic", palette)

    supplemental = dicom_file.supplemental_palette
    if supplemental is not None:
        return ("supplemental", supplemental)

    return None


def _normalize_palette_bits_per_entry(palette: _dicomsdl.PaletteLut) -> int:
    bits_per_entry = int(palette.bits_per_entry)
    if bits_per_entry > 0:
        return bits_per_entry

    max_value = 0
    for channel_values in (
        palette.red_values,
        palette.green_values,
        palette.blue_values,
        palette.alpha_values,
    ):
        if channel_values:
            max_value = max(max_value, max(int(value) for value in channel_values))
    return 8 if max_value <= 0xFF else 16


def _cast_palette_indices(indices, bits_mapped: int):
    try:
        import numpy as np
    except ImportError as exc:  # pragma: no cover - numpy is required for to_array anyway.
        raise ImportError("DicomFile.to_pil_image requires NumPy to be installed.") from exc

    if bits_mapped <= 8:
        return indices.astype(np.uint8, copy=False)
    if bits_mapped <= 16:
        return indices.astype(np.uint16, copy=False)
    return indices.astype(np.uint32, copy=False)


def _render_supported_enhanced_palette(
    dicom_file: _dicomsdl.DicomFile,
    frame: int,
    array,
    enhanced: _dicomsdl.EnhancedPaletteInfo,
):
    try:
        import numpy as np
    except ImportError as exc:  # pragma: no cover - numpy is required for to_array anyway.
        raise ImportError("DicomFile.to_pil_image requires NumPy to be installed.") from exc

    if array.ndim != 2 or not np.issubdtype(array.dtype, np.integer):
        raise NotImplementedError(
            "to_pil_image currently supports Enhanced Palette only for 2D integral source arrays."
        )
    if enhanced.has_blending_lut_1 or enhanced.has_blending_lut_2:
        raise NotImplementedError(
            "to_pil_image does not yet support Enhanced Palette blending LUT stages."
        )
    if enhanced.color_space and enhanced.color_space.upper() != "SRGB":
        raise NotImplementedError(
            "to_pil_image currently supports only sRGB Enhanced Palette output."
        )
    if len(enhanced.data_frame_assignments) != 1 or len(enhanced.palette_items) != 1:
        raise NotImplementedError(
            "to_pil_image currently supports only single-path Enhanced Palette pipelines."
        )

    assignment = enhanced.data_frame_assignments[0]
    palette_item = enhanced.palette_items[0]
    if assignment.data_path_assignment.upper() != "PRIMARY_SINGLE":
        raise NotImplementedError(
            "to_pil_image currently supports only PRIMARY_SINGLE Enhanced Palette assignment."
        )
    if palette_item.data_path_id.upper() != "PRIMARY":
        raise NotImplementedError(
            "to_pil_image currently supports only PRIMARY Enhanced Palette data paths."
        )

    rgb_tf = palette_item.rgb_lut_transfer_function.upper()
    if rgb_tf and rgb_tf != "IDENTITY":
        raise NotImplementedError(
            "to_pil_image currently supports only IDENTITY RGB LUT transfer functions."
        )
    alpha_tf = palette_item.alpha_lut_transfer_function.upper()
    if alpha_tf and alpha_tf != "IDENTITY":
        raise NotImplementedError(
            "to_pil_image currently supports only IDENTITY alpha LUT transfer functions."
        )

    if (
        dicom_file.modality_lut_for_frame(frame) is not None
        or dicom_file.rescale_transform_for_frame(frame) is not None
        or dicom_file.voi_lut_for_frame(frame) is not None
        or dicom_file.window_transform_for_frame(frame) is not None
    ):
        raise NotImplementedError(
            "to_pil_image does not yet support Enhanced Palette pipelines with modality/VOI transforms."
        )

    ds = dicom_file.dataset
    bits_stored = _get_tag_int(ds, "BitsStored")
    if bits_stored is None:
        bits_stored = int(array.dtype.itemsize * 8)
    if bits_stored <= 0 or bits_stored > 32:
        raise NotImplementedError(
            "to_pil_image currently supports Enhanced Palette only up to 32 stored bits."
        )

    bits_mapped = (
        int(assignment.bits_mapped_to_color_lookup_table)
        if assignment.has_bits_mapped_to_color_lookup_table
        else bits_stored
    )
    if bits_mapped <= 0 or bits_mapped > bits_stored:
        raise ValueError("Enhanced Palette BitsMappedToColorLookupTable is invalid.")

    mask = (1 << bits_stored) - 1
    mapped = np.bitwise_and(array.astype(np.int64, copy=False), mask)
    if bits_mapped < bits_stored:
        mapped = np.right_shift(mapped, bits_stored - bits_mapped)

    mapped = _cast_palette_indices(mapped, bits_mapped)
    return _dicomsdl.apply_palette_lut(mapped, palette_item.palette)


def _palette_result_mode(color) -> str:
    channels = int(color.shape[2])
    if channels == 3:
        return "RGB"
    if channels == 4:
        return "RGBA"
    raise ValueError(f"unsupported palette output shape for to_pil_image: {color.shape}")


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


def _render_display_array(
    self: _dicomsdl.DicomFile,
    frame: int = 0,
    *,
    window: tuple[float, float] | None = None,
    auto_window: bool = True,
):
    if frame < 0:
        raise ValueError("frame must be >= 0")

    ds = self.dataset
    array = self.to_array(frame=frame)
    photometric = _get_tag_upper_text(ds, "PhotometricInterpretation")
    if array.ndim == 2:
        display_palette = _resolve_display_palette(self, photometric)
        if display_palette is not None:
            kind, palette_info = display_palette
            if kind == "enhanced":
                color = _render_supported_enhanced_palette(self, frame, array, palette_info)
                color = _normalize_palette_to_uint8(
                    color, _normalize_palette_bits_per_entry(palette_info.palette_items[0].palette)
                )
                return color

            palette = (
                palette_info
                if kind == "classic"
                else palette_info.palette
            )
            color = _dicomsdl.apply_palette_lut(array, palette)
            color = _normalize_palette_to_uint8(color, int(palette.bits_per_entry))
            return color

        display_window = _resolve_window_transform(self, frame, window, auto_window)
        display_voi_lut = _resolve_voi_lut(self, frame, window, auto_window)
        mono = _normalize_mono_to_uint8(array, display_window, display_voi_lut)
        if photometric == "MONOCHROME1":
            mono = 255 - mono
        return mono

    if array.ndim == 3:
        channels = int(array.shape[2])
        if channels not in (3, 4):
            raise ValueError(f"unsupported color shape for to_pil_image: {array.shape}")

        color = _normalize_color_to_uint8(array)
        color = _convert_ybr_to_rgb(color, photometric)
        return color

    raise ValueError(f"unsupported pixel array ndim for to_pil_image: {array.ndim}")


def _dicomfile_to_pil_image(
    self: _dicomsdl.DicomFile,
    frame: int = 0,
    *,
    window: tuple[float, float] | None = None,
    auto_window: bool = True,
):
    try:
        from PIL import Image
    except ImportError as exc:
        raise ImportError(
            "DicomFile.to_pil_image requires Pillow. Install with 'pip install pillow'."
        ) from exc

    rendered = _render_display_array(
        self, frame=frame, window=window, auto_window=auto_window
    )
    if rendered.ndim == 2:
        return Image.fromarray(rendered, mode="L")
    return Image.fromarray(rendered, mode=_palette_result_mode(rendered))


_dicomsdl.DicomFile.to_pil_image = _dicomfile_to_pil_image
