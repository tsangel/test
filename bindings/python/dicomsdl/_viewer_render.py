from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

import numpy as np

from . import Tag, Uid, apply_rescale, is_dicom_file, read_file
from ._image import _get_tag_upper_text, _render_display_array
from ._viewer_display import DisplaySettings, format_display_value, modality_group
from ._qt_compat import QtGui

METADATA_LOAD_UNTIL = Tag("0028,1053")


class ViewerRenderError(RuntimeError):
    pass


class NonDicomFileError(ViewerRenderError):
    pass


class DicomNotDisplayableError(ViewerRenderError):
    pass


@dataclass(frozen=True, slots=True)
class RenderedFrame:
    pixmap: QtGui.QPixmap
    frame_index: int
    frame_count: int
    rows: int
    columns: int
    bits_allocated: int | None
    bits_stored: int | None
    high_bit: int | None
    pixel_representation: int | None
    raw_dtype: str | None
    raw_min: float | int | None
    raw_max: float | int | None
    source_min: float | int | None
    source_max: float | int | None
    sop_class: str
    sop_class_uid_value: str
    photometric: str
    transfer_syntax: str
    transfer_syntax_uid_value: str
    dicom_window_text: str
    rescale_text: str
    display_text: str
    display_min: float | None
    display_max: float | None
    display_adjustable: bool


def _tag_int(dicom_file, keyword: str, default: int | None = None) -> int | None:
    value = dicom_file.get_dataelement(keyword).to_long()
    if value is None:
        return default
    return int(value)


def _transfer_syntax_text(dicom_file) -> str:
    uid = dicom_file.transfer_syntax_uid
    return _uid_display_text(uid)


def _transfer_syntax_value(dicom_file) -> str:
    uid = dicom_file.transfer_syntax_uid
    if not uid:
        return "-"
    return str(uid.value)


def _uid_display_text(uid) -> str:
    if not uid:
        return "-"
    if uid.name:
        return str(uid.name)
    if uid.keyword:
        return str(uid.keyword)
    return str(uid.value)


def _sop_class_text(dicom_file) -> tuple[str, str]:
    for keyword in ("SOPClassUID", "MediaStorageSOPClassUID"):
        try:
            element = dicom_file.get_dataelement(keyword)
            value = element.to_uid_string() or element.to_string_view()
        except Exception:
            continue
        if not value:
            continue
        uid = Uid.lookup(value)
        if uid is not None:
            return (_uid_display_text(uid), str(uid.value))
        return (str(value), str(value))
    return ("-", "-")


def _window_bounds(center: float, width: float) -> tuple[float, float]:
    width = max(float(width), 1e-6)
    lower = float(center) - 0.5 - (width - 1.0) * 0.5
    upper = float(center) - 0.5 + (width - 1.0) * 0.5
    if upper <= lower:
        upper = lower + 1.0
    return (lower, upper)


def _format_window_text(center: float, width: float) -> str:
    return f"{int(round(width))}/{int(round(center))}"


def _format_range_text(lower: float, upper: float) -> str:
    return f"{format_display_value(lower)} .. {format_display_value(upper)}"


def _dicom_window_text(dicom_file, frame_index: int) -> str:
    window = dicom_file.window_transform_for_frame(frame_index)
    if window is None:
        return "-"
    return _format_window_text(window.center, window.width)


def _rescale_text(dicom_file, frame_index: int) -> str:
    transform = dicom_file.rescale_transform_for_frame(frame_index)
    if transform is None:
        return "-"
    slope = format_display_value(transform.slope)
    intercept = format_display_value(transform.intercept)
    return f"{slope} / {intercept}"


def _normalize_range_to_uint8(
    array: np.ndarray,
    lower: float,
    upper: float,
) -> np.ndarray:
    if upper <= lower:
        return np.zeros(array.shape, dtype=np.uint8)
    work = array.astype(np.float32, copy=False)
    scaled = (work - float(lower)) * (255.0 / float(upper - lower))
    scaled = np.nan_to_num(scaled, nan=0.0, posinf=255.0, neginf=0.0)
    return np.clip(scaled, 0.0, 255.0).astype(np.uint8)


def _decoded_display_array(dicom_file, frame_index: int) -> np.ndarray:
    array = dicom_file.to_array(frame=frame_index)
    if array.ndim != 2:
        return array
    transform = dicom_file.rescale_transform_for_frame(frame_index)
    if transform is not None:
        array = apply_rescale(array, transform)
    return array


def _finite_percentile(values: np.ndarray, percentile: float) -> float:
    finite = values[np.isfinite(values)]
    if finite.size == 0:
        return 0.0
    return float(np.percentile(finite, percentile))


def _finite_min_max(values: np.ndarray) -> tuple[float | int | None, float | int | None]:
    finite = values[np.isfinite(values)]
    if finite.size == 0:
        return (None, None)
    lower = finite.min()
    upper = finite.max()
    if np.issubdtype(finite.dtype, np.integer):
        return (int(lower), int(upper))
    return (float(lower), float(upper))


def _resolve_display_range(
    dicom_file,
    frame_index: int,
    array: np.ndarray,
    display: DisplaySettings,
    modality: str | None,
) -> tuple[float, float, str]:
    group = modality_group(modality)
    mode = display.mode or "auto"

    if mode == "window" and display.center is not None and display.width is not None:
        lower, upper = _window_bounds(display.center, display.width)
        return (lower, upper, _format_window_text(display.center, display.width))

    if mode == "range" and display.minimum is not None and display.maximum is not None:
        lower = float(display.minimum)
        upper = float(display.maximum)
        if upper <= lower:
            upper = lower + 1.0
        return (lower, upper, _format_range_text(lower, upper))

    if mode == "percentile":
        low_percentile = 0.0 if display.low_percentile is None else float(display.low_percentile)
        high_percentile = 99.5 if display.high_percentile is None else float(display.high_percentile)
        lower = _finite_percentile(array, low_percentile)
        upper = _finite_percentile(array, high_percentile)
        if display.clamp_min_zero:
            lower = max(0.0, lower)
        if upper <= lower:
            upper = float(np.nanmax(array)) if array.size != 0 else lower + 1.0
        text = display.label or f"P{low_percentile:g}-P{high_percentile:g}"
        if display.clamp_min_zero:
            text = display.label or f"0-P{high_percentile:g}"
        return (lower, max(upper, lower + 1.0), text)

    if mode == "dicom":
        window = dicom_file.window_transform_for_frame(frame_index)
        if window is not None:
            lower, upper = _window_bounds(window.center, window.width)
            return (lower, upper, _format_window_text(window.center, window.width))

    if group == "nuclear":
        upper = _finite_percentile(array, 90.0)
        upper = max(upper, 1.0)
        return (0.0, upper, "0-P90")

    window = dicom_file.window_transform_for_frame(frame_index)
    if window is not None:
        lower, upper = _window_bounds(window.center, window.width)
        return (lower, upper, _format_window_text(window.center, window.width))

    if group == "mr":
        lower = _finite_percentile(array, 10.0)
        upper = _finite_percentile(array, 90.0)
        if upper <= lower:
            upper = lower + 1.0
        return (lower, upper, "P10-P90")

    lower = _finite_percentile(array, 10.0)
    upper = _finite_percentile(array, 90.0)
    if upper <= lower:
        lower = float(np.nanmin(array)) if array.size != 0 else 0.0
        upper = float(np.nanmax(array)) if array.size != 0 else 1.0
    if upper <= lower:
        upper = lower + 1.0
    return (lower, upper, "P10-P90")


def _to_qimage(array: np.ndarray) -> QtGui.QImage:
    if array.ndim == 2:
        height, width = array.shape
        image = QtGui.QImage(
            array.data,
            width,
            height,
            int(array.strides[0]),
            QtGui.QImage.Format.Format_Grayscale8,
        )
        return image.copy()

    height, width, channels = array.shape
    if channels == 3:
        image = QtGui.QImage(
            array.data,
            width,
            height,
            int(array.strides[0]),
            QtGui.QImage.Format.Format_RGB888,
        )
        return image.copy()
    if channels == 4:
        image = QtGui.QImage(
            array.data,
            width,
            height,
            int(array.strides[0]),
            QtGui.QImage.Format.Format_RGBA8888,
        )
        return image.copy()
    raise ValueError("Unsupported channel count for dicomview.")


def load_dicom_for_view(path: Path):
    if not is_dicom_file(path):
        raise NonDicomFileError("Not a DICOM file.")
    try:
        return read_file(path, load_until=METADATA_LOAD_UNTIL)
    except Exception as exc:
        raise DicomNotDisplayableError(str(exc)) from exc


def render_loaded_dicom(
    dicom_file,
    frame_index: int = 0,
    *,
    display: DisplaySettings | None = None,
    modality: str | None = None,
) -> RenderedFrame:
    try:
        dicom_file.ensure_loaded("PixelData")
    except Exception as exc:
        raise DicomNotDisplayableError(str(exc)) from exc

    frame_count = _tag_int(dicom_file, "NumberOfFrames", 1) or 1
    frame_index = max(0, min(frame_index, frame_count - 1))
    display = display or DisplaySettings.auto()
    photometric = _get_tag_upper_text(dicom_file.dataset, "PhotometricInterpretation")

    display_min: float | None = None
    display_max: float | None = None
    display_text = "n/a"
    display_adjustable = False

    try:
        raw_source = dicom_file.to_array(frame=frame_index)
        raw_dtype = str(raw_source.dtype)
        raw_min, raw_max = _finite_min_max(np.asarray(raw_source))
        source = raw_source
        if raw_source.ndim == 2:
            transform = dicom_file.rescale_transform_for_frame(frame_index)
            if transform is not None:
                source = apply_rescale(raw_source, transform)
        source_min, source_max = _finite_min_max(np.asarray(source))
        if source.ndim == 2 and photometric != "PALETTE COLOR":
            lower, upper, display_text = _resolve_display_range(
                dicom_file,
                frame_index,
                source,
                display,
                modality,
            )
            rendered = _normalize_range_to_uint8(source, lower, upper)
            if photometric == "MONOCHROME1":
                rendered = 255 - rendered
            display_min = lower
            display_max = upper
            display_adjustable = True
        else:
            rendered = np.ascontiguousarray(
                _render_display_array(dicom_file, frame=frame_index, auto_window=True)
            )
    except Exception as exc:
        raise DicomNotDisplayableError(str(exc)) from exc

    rows = _tag_int(dicom_file, "Rows", int(rendered.shape[0])) or int(rendered.shape[0])
    columns = _tag_int(dicom_file, "Columns", int(rendered.shape[1])) or int(rendered.shape[1])
    bits_allocated = _tag_int(dicom_file, "BitsAllocated")
    bits_stored = _tag_int(dicom_file, "BitsStored")
    high_bit = _tag_int(dicom_file, "HighBit")
    pixel_representation = _tag_int(dicom_file, "PixelRepresentation")
    transfer_syntax = _transfer_syntax_text(dicom_file)
    transfer_syntax_uid_value = _transfer_syntax_value(dicom_file)
    sop_class, sop_class_uid_value = _sop_class_text(dicom_file)
    dicom_window_text = _dicom_window_text(dicom_file, frame_index)
    rescale_text = _rescale_text(dicom_file, frame_index)

    try:
        qimage = _to_qimage(rendered)
    except Exception as exc:
        raise DicomNotDisplayableError(str(exc)) from exc
    pixmap = QtGui.QPixmap.fromImage(qimage)
    return RenderedFrame(
        pixmap=pixmap,
        frame_index=frame_index,
        frame_count=frame_count,
        rows=rows,
        columns=columns,
        bits_allocated=bits_allocated,
        bits_stored=bits_stored,
        high_bit=high_bit,
        pixel_representation=pixel_representation,
        raw_dtype=raw_dtype,
        raw_min=raw_min,
        raw_max=raw_max,
        source_min=source_min,
        source_max=source_max,
        sop_class=sop_class,
        sop_class_uid_value=sop_class_uid_value,
        photometric=photometric or "-",
        transfer_syntax=transfer_syntax,
        transfer_syntax_uid_value=transfer_syntax_uid_value,
        dicom_window_text=dicom_window_text,
        rescale_text=rescale_text,
        display_text=display_text,
        display_min=display_min,
        display_max=display_max,
        display_adjustable=display_adjustable,
    )


def render_dicom_path(
    path: Path,
    frame_index: int = 0,
    *,
    display: DisplaySettings | None = None,
    modality: str | None = None,
) -> RenderedFrame:
    dicom_file = load_dicom_for_view(path)
    return render_loaded_dicom(
        dicom_file,
        frame_index=frame_index,
        display=display,
        modality=modality,
    )
