#!/usr/bin/env python3
"""Shared helpers for PET series reference examples."""

from __future__ import annotations

import argparse
from pathlib import Path


def default_pet_root() -> Path:
    return Path(__file__).resolve().parents[4] / "sample" / "pt"


def add_series_selection_args(parser: argparse.ArgumentParser) -> None:
    default_path = default_pet_root()
    parser.add_argument(
        "path",
        nargs="?",
        default=str(default_path),
        help=(
            "Path to a DICOM series directory or a parent directory containing "
            f"series subdirectories (default: {default_path})"
        ),
    )
    parser.add_argument(
        "--series-index",
        type=int,
        default=0,
        help="Series index to use when the selected path contains multiple subdirectories (default: 0)",
    )
    parser.add_argument(
        "--series-name",
        help="Case-insensitive substring used to pick a series directory by name",
    )


def discover_series_dirs(path: Path) -> list[Path]:
    if not path.exists():
        raise FileNotFoundError(f"Path does not exist: {path}")
    if not path.is_dir():
        raise NotADirectoryError(f"Path is not a directory: {path}")

    direct_files = sorted(child for child in path.iterdir() if child.is_file() and child.suffix.lower() == ".dcm")
    if direct_files:
        return [path]

    series_dirs = []
    for child in sorted(path.iterdir()):
        if not child.is_dir():
            continue
        if any(grandchild.is_file() and grandchild.suffix.lower() == ".dcm" for grandchild in child.iterdir()):
            series_dirs.append(child)
    return series_dirs


def resolve_series_dir(path_text: str, series_index: int, series_name: str | None) -> tuple[Path, list[Path]]:
    path = Path(path_text).expanduser().resolve()
    series_dirs = discover_series_dirs(path)
    if not series_dirs:
        raise FileNotFoundError(f"No DICOM series directories found under: {path}")

    if series_name:
        needle = series_name.casefold()
        matched = [series_dir for series_dir in series_dirs if needle in series_dir.name.casefold()]
        if not matched:
            available = ", ".join(series_dir.name for series_dir in series_dirs)
            raise ValueError(
                f"No series matched --series-name={series_name!r}. Available series: {available}"
            )
        return matched[0], series_dirs

    if series_index < 0 or series_index >= len(series_dirs):
        raise IndexError(
            f"--series-index must be between 0 and {len(series_dirs) - 1} (got {series_index})"
        )
    return series_dirs[series_index], series_dirs


def count_dicom_files(series_dir: Path) -> int:
    return sum(1 for child in series_dir.iterdir() if child.is_file() and child.suffix.lower() == ".dcm")
