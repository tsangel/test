from __future__ import annotations

import sys

from ._qt_compat import QtGui


def _general_font_families() -> list[str]:
    if sys.platform == "darwin":
        return [
            "Helvetica Neue",
            "SF Pro Text",
            "Arial",
            "Apple SD Gothic Neo",
        ]
    if sys.platform.startswith("win"):
        return [
            "Segoe UI Variable Text",
            "Segoe UI",
            "Arial",
        ]
    return [
        "Inter",
        "Noto Sans",
        "Ubuntu",
        "Cantarell",
        "DejaVu Sans",
    ]


def _fixed_font_families() -> list[str]:
    if sys.platform == "darwin":
        return [
            "Menlo",
            "SF Mono",
            "Monaco",
            "Courier New",
        ]
    if sys.platform.startswith("win"):
        return [
            "Cascadia Mono",
            "Consolas",
            "Courier New",
        ]
    return [
        "JetBrains Mono",
        "Noto Sans Mono",
        "DejaVu Sans Mono",
        "monospace",
    ]


def general_ui_font(point_size: float = 12.0) -> QtGui.QFont:
    font = QtGui.QFont()
    font.setFamilies(_general_font_families())
    font.setPointSizeF(point_size)
    return font


def fixed_ui_font(point_size: float = 11.0) -> QtGui.QFont:
    font = QtGui.QFont()
    font.setFamilies(_fixed_font_families())
    font.setPointSizeF(point_size)
    return font
