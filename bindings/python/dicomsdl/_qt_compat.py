from __future__ import annotations

try:
    from PySide6 import QtCore, QtGui, QtWidgets
except ImportError as exc:  # pragma: no cover - depends on optional viewer extra.
    raise ImportError(
        "dicomview requires PySide6. Install with 'pip install \"dicomsdl[viewer]\"'."
    ) from exc

Signal = QtCore.Signal
Slot = QtCore.Slot

__all__ = ["QtCore", "QtGui", "QtWidgets", "Signal", "Slot"]
