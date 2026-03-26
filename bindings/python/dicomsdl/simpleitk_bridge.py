from __future__ import annotations

from pathlib import Path

from ._bridge_common import SeriesVolume, read_series_volume


def _load_simpleitk():
    try:
        import SimpleITK as sitk
    except ImportError as exc:
        raise RuntimeError(
            "SimpleITK is not installed. Install it with: pip install SimpleITK"
        ) from exc
    return sitk


def to_simpleitk_image(volume: SeriesVolume):
    sitk = _load_simpleitk()
    is_vector = volume.array.ndim == 4
    image = sitk.GetImageFromArray(volume.array, isVector=is_vector)
    image.SetSpacing(volume.spacing)
    image.SetOrigin(volume.origin)
    image.SetDirection(volume.direction)
    return image


def read_series_image(
    path: str | Path,
    *,
    to_modality_value: bool = True,
):
    return to_simpleitk_image(
        read_series_volume(path, to_modality_value=to_modality_value)
    )


__all__ = (
    "SeriesVolume",
    "read_series_volume",
    "to_simpleitk_image",
    "read_series_image",
)
