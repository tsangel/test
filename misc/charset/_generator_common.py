from __future__ import annotations

import pathlib


def write_text_if_changed(path: pathlib.Path, content: str) -> bool:
    if path.exists():
        existing = path.read_text(encoding="utf-8")
        if existing == content:
            return False
    path.write_text(content, encoding="utf-8", newline="\n")
    return True
