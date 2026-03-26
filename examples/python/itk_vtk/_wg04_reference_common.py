#!/usr/bin/env python3
"""Shared helpers for WG04 single-file 2D DICOM examples."""

from __future__ import annotations

import argparse
import ftplib
import os
from pathlib import Path
import tarfile
import tempfile
import time

_WG04_IMAGES_ENV = "DICOMSDL_WG04_IMAGES_BASE"
_WG04_ARCHIVE_NAME = "compsamples_refanddir.tar.bz2"
_WG04_FTP_DIR = "/MEDICAL/Dicom/DataSets/WG04"
_WG04_FTP_HOSTS = ("medical.nema.org", "dicom.nema.org")


def default_wg04_images_root() -> Path:
    override = os.environ.get(_WG04_IMAGES_ENV)
    if override:
        return Path(override).expanduser().resolve()
    return Path(__file__).resolve().parents[4] / "sample" / "nema" / "WG04" / "IMAGES"


def default_wg04_root() -> Path:
    return default_wg04_images_root() / "REF"


def _downloadable_wg04_paths(path: Path) -> tuple[Path, Path] | None:
    if path == default_wg04_root():
        return (default_wg04_images_root(), default_wg04_root())
    if path == default_wg04_images_root():
        return (path, path / "REF")
    if path.name.upper() == "REF":
        return (path.parent, path)
    if path.name.upper() == "IMAGES":
        return (path, path / "REF")
    if path.parent.name.upper() == "REF":
        return (path.parent.parent, path)
    return None


def _format_size(num_bytes: int) -> str:
    value = float(num_bytes)
    for unit in ("B", "KiB", "MiB", "GiB"):
        if value < 1024.0 or unit == "GiB":
            return f"{value:.1f}{unit}"
        value /= 1024.0
    return f"{value:.1f}GiB"


def _format_duration(seconds: float) -> str:
    total_seconds = max(0, int(round(seconds)))
    minutes, secs = divmod(total_seconds, 60)
    hours, minutes = divmod(minutes, 60)
    if hours > 0:
        return f"{hours:d}:{minutes:02d}:{secs:02d}"
    return f"{minutes:02d}:{secs:02d}"


def _progress_text(
    label: str,
    transferred: int,
    *,
    total_bytes: int | None,
    start_time: float,
) -> str:
    elapsed = max(time.monotonic() - start_time, 1e-9)
    speed = transferred / elapsed
    speed_text = f"{_format_size(int(speed))}/s"
    if total_bytes is not None and total_bytes > 0:
        percent = (transferred / total_bytes) * 100.0
        remaining = max(total_bytes - transferred, 0)
        eta_seconds = remaining / speed if speed > 0 else 0.0
        return (
            f"Downloading {label}: {percent:6.2f}% "
            f"({_format_size(transferred)}/{_format_size(total_bytes)}) "
            f"{speed_text} ETA {_format_duration(eta_seconds)}"
        )
    return f"Downloading {label}: {_format_size(transferred)} {speed_text}"


def _download_with_progress(
    ftp: ftplib.FTP,
    remote_name: str,
    destination_path: Path,
    *,
    blocksize: int,
) -> None:
    try:
        total_bytes = ftp.size(remote_name)
    except Exception:
        total_bytes = None

    transferred = 0
    last_report = 0.0
    start_time = time.monotonic()
    label = destination_path.name

    def callback(chunk: bytes) -> None:
        nonlocal transferred, last_report
        handle.write(chunk)
        transferred += len(chunk)
        now = time.monotonic()
        if now - last_report < 0.5 and (total_bytes is None or transferred < total_bytes):
            return
        text = _progress_text(
            label,
            transferred,
            total_bytes=total_bytes,
            start_time=start_time,
        )
        print(text, end="\r", flush=True)
        last_report = now

    with destination_path.open("wb") as handle:
        ftp.retrbinary(f"RETR {remote_name}", callback, blocksize=blocksize)
    elapsed = max(time.monotonic() - start_time, 1e-9)
    speed_text = f"{_format_size(int(transferred / elapsed))}/s"
    if total_bytes is not None and total_bytes > 0:
        print(
            f"Downloaded {label}: 100.00% "
            f"({_format_size(total_bytes)}/{_format_size(total_bytes)}) "
            f"{speed_text} elapsed {_format_duration(elapsed)}"
        )
    else:
        print(
            f"Downloaded {label}: {_format_size(transferred)} "
            f"{speed_text} elapsed {_format_duration(elapsed)}"
        )


def _download_wg04_archive(archive_path: Path) -> None:
    errors: list[str] = []
    for host in _WG04_FTP_HOSTS:
        try:
            with ftplib.FTP(host, timeout=30) as ftp:
                ftp.login()
                ftp.cwd(_WG04_FTP_DIR)
                _download_with_progress(
                    ftp,
                    _WG04_ARCHIVE_NAME,
                    archive_path,
                    blocksize=1 << 20,
                )
            return
        except Exception as exc:
            errors.append(f"{host}: {exc}")
    joined = "; ".join(errors)
    raise RuntimeError(f"Unable to download WG04 samples from NEMA FTP ({joined})")


def _safe_extract_tar_bz2(archive_path: Path, destination_dir: Path) -> None:
    destination_dir = destination_dir.resolve()
    with tarfile.open(archive_path, mode="r:bz2") as archive:
        members = archive.getmembers()
        for member in members:
            member_path = (destination_dir / member.name).resolve()
            member_path.relative_to(destination_dir)
        archive.extractall(destination_dir)


def _maybe_download_wg04_samples(path: Path, *, download_missing: bool) -> Path:
    if path.exists() or not download_missing:
        return path

    inferred = _downloadable_wg04_paths(path)
    if inferred is None:
        raise FileNotFoundError(
            f"Path does not exist: {path}. Automatic WG04 download only supports the default WG04 root "
            f"or paths ending in IMAGES/REF. Set {_WG04_IMAGES_ENV} or pass a WG04-style path."
        )

    images_root, resolved_path = inferred
    ref_root = images_root / "REF"
    if ref_root.is_dir():
        return resolved_path

    images_root.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="wg04-download-", dir=str(images_root.parent)) as temp_dir:
        archive_path = Path(temp_dir) / _WG04_ARCHIVE_NAME
        _download_wg04_archive(archive_path)
        _safe_extract_tar_bz2(archive_path, images_root.parent)

    if not ref_root.is_dir():
        raise RuntimeError(f"WG04 download completed but REF samples were not extracted under {ref_root}")

    return resolved_path


def add_sample_selection_args(parser: argparse.ArgumentParser) -> None:
    default_path = default_wg04_root()
    parser.add_argument(
        "path",
        nargs="?",
        default=str(default_path),
        help=(
            "Path to a WG04 DICOM file or a directory containing WG04 sample files "
            f"(default: {default_path})"
        ),
    )
    parser.add_argument(
        "--sample-index",
        type=int,
        default=0,
        help="Sample index to use when the selected path is a directory (default: 0)",
    )
    parser.add_argument(
        "--sample-name",
        help="Case-insensitive substring used to pick a sample file by name",
    )
    parser.add_argument(
        "--download-missing",
        action="store_true",
        help=(
            "If the selected WG04 path is missing, download the official NEMA REF archive "
            f"into the default or inferred WG04 IMAGES root (override root with {_WG04_IMAGES_ENV})."
        ),
    )


def discover_sample_files(path: Path) -> list[Path]:
    if not path.exists():
        raise FileNotFoundError(f"Path does not exist: {path}")
    if path.is_file():
        return [path]
    if not path.is_dir():
        raise NotADirectoryError(f"Path is not a directory: {path}")

    return sorted(child for child in path.iterdir() if child.is_file())


def resolve_sample_file(
    path_text: str,
    sample_index: int,
    sample_name: str | None,
    *,
    download_missing: bool = False,
) -> tuple[Path, list[Path]]:
    path = _maybe_download_wg04_samples(
        Path(path_text).expanduser().resolve(),
        download_missing=download_missing,
    )
    sample_files = discover_sample_files(path)
    if not sample_files:
        raise FileNotFoundError(f"No sample files found under: {path}")

    if sample_name:
        needle = sample_name.casefold()
        matched = [sample_file for sample_file in sample_files if needle in sample_file.name.casefold()]
        if not matched:
            available = ", ".join(sample_file.name for sample_file in sample_files)
            raise ValueError(
                f"No sample matched --sample-name={sample_name!r}. Available samples: {available}"
            )
        return matched[0], sample_files

    if sample_index < 0 or sample_index >= len(sample_files):
        raise IndexError(
            f"--sample-index must be between 0 and {len(sample_files) - 1} (got {sample_index})"
        )

    return sample_files[sample_index], sample_files
