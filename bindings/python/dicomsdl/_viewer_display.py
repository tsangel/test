from __future__ import annotations

from dataclasses import dataclass


def modality_group(modality: str | None) -> str:
    text = (modality or "").strip().upper()
    if text in {"PT", "NM"}:
        return "nuclear"
    if text == "MR":
        return "mr"
    if text == "CT":
        return "ct"
    return "generic"


def format_display_value(value: float) -> str:
    magnitude = abs(value)
    if magnitude >= 1000.0:
        return f"{value:.0f}"
    if magnitude >= 100.0:
        return f"{value:.1f}"
    if magnitude >= 10.0:
        return f"{value:.2f}".rstrip("0").rstrip(".")
    if magnitude >= 1.0:
        return f"{value:.3f}".rstrip("0").rstrip(".")
    return f"{value:.4f}".rstrip("0").rstrip(".")


@dataclass(slots=True)
class DisplaySettings:
    mode: str = "auto"
    label: str = "Auto"
    center: float | None = None
    width: float | None = None
    minimum: float | None = None
    maximum: float | None = None
    low_percentile: float | None = None
    high_percentile: float | None = None
    clamp_min_zero: bool = False

    @classmethod
    def auto(cls) -> "DisplaySettings":
        return cls()

    @classmethod
    def dicom_window(cls) -> "DisplaySettings":
        return cls(mode="dicom", label="DICOM")

    @classmethod
    def window(cls, center: float, width: float, label: str = "Manual") -> "DisplaySettings":
        return cls(mode="window", label=label, center=float(center), width=float(width))

    @classmethod
    def value_range(
        cls,
        minimum: float,
        maximum: float,
        label: str = "Manual",
    ) -> "DisplaySettings":
        return cls(
            mode="range",
            label=label,
            minimum=float(minimum),
            maximum=float(maximum),
        )

    @classmethod
    def percentile_range(
        cls,
        low_percentile: float,
        high_percentile: float,
        *,
        clamp_min_zero: bool = False,
        label: str,
    ) -> "DisplaySettings":
        return cls(
            mode="percentile",
            label=label,
            low_percentile=float(low_percentile),
            high_percentile=float(high_percentile),
            clamp_min_zero=bool(clamp_min_zero),
        )
