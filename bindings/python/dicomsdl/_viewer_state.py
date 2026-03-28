from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path

from ._viewer_display import DisplaySettings


@dataclass(slots=True)
class ViewerState:
    current_dir: Path
    selected_file: Path | None = None
    current_frame: int = 0
    display: DisplaySettings = field(default_factory=DisplaySettings.auto)
