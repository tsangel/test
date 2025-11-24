#!/usr/bin/env python3
"""Simple benchmark: read all DICOM files under a directory using dicomsdl."""

from __future__ import annotations

import argparse
import pathlib
import sys
import time
from typing import Iterable

import importlib


def find_dcm_files(root: pathlib.Path) -> Iterable[pathlib.Path]:
    # Treat .dcm (case-insensitive) as DICOM candidates.
    for path in root.rglob("*"):
        if path.is_file() and path.suffix.lower() == ".dcm":
            yield path


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Read every .dcm file under the given directory with dicomsdl.read_file",
    )
    parser.add_argument(
        "root",
        nargs="?",
        type=pathlib.Path,
        default=pathlib.Path("/Users/tsangel/Documents/workspace.dev/sample/ncc/3121/pt"),
        help="Root directory to scan recursively for .dcm files",
    )
    parser.add_argument(
        "--impl",
        choices=["new", "old", "pydicom", "gdcm"],
        default="new",
        help="Which backend to use: new (current repo build), old (PyPI dicomsdl), pydicom, or gdcm",
    )
    parser.add_argument(
        "--repeat",
        "-r",
        type=int,
        default=10,
        help="How many times to repeat reading the whole set (default: 10)",
    )
    parser.add_argument(
        "--verbose",
        "-v",
        action="store_true",
        help="Print per-file timings",
    )
    args = parser.parse_args(argv)

    print(f"impl={args.impl} repeat={args.repeat} root={args.root}")

    def get_reader(kind: str):
        if kind == "new":
            dicom = importlib.import_module("dicomsdl")
            return lambda path: dicom.read_file(path)
        if kind == "old":
            # Expect the old wheel to be importable as `dicomsdl_old` or fallback to `dicomsdl`.
            dicom = importlib.import_module("dicomsdl")
            return lambda path: dicom.open_file(path)
        if kind == "pydicom":
            pydicom = importlib.import_module("pydicom")
            return lambda path: pydicom.dcmread(path, force=True)
        if kind == "gdcm":
            gdcm = importlib.import_module("gdcm")

            def _read(path: str):
                reader = gdcm.Reader()
                reader.SetFileName(path)
                if not reader.Read():
                    raise RuntimeError(f"gdcm failed to read {path}")
                return reader

            return _read
        raise ValueError(f"unknown impl: {kind}")

    read_func = get_reader(args.impl)

    root = args.root
    if not root.exists():
        print(f"root does not exist: {root}", file=sys.stderr)
        return 1

    files = sorted(find_dcm_files(root))
    if not files:
        print(f"no .dcm files found under: {root}")
        return 0

    files_with_sizes = [(p, p.stat().st_size) for p in files]
    bytes_per_run = sum(size for _, size in files_with_sizes)

    total_seconds = 0.0
    run_times: list[float] = []

    for run in range(args.repeat):
        run_seconds = 0.0
        for path, size in files_with_sizes:
            t0 = time.perf_counter()
            _ = read_func(str(path))
            t1 = time.perf_counter()
            elapsed = t1 - t0
            run_seconds += elapsed
            if args.verbose:
                print(f"[{run + 1}/{args.repeat}] {path} size={size}B time={elapsed*1000:.2f} ms")
        run_times.append(run_seconds)
        total_seconds += run_seconds
        run_avg_ms = (run_seconds / len(files_with_sizes)) * 1000.0
        run_mbps = (bytes_per_run / (1024 * 1024)) / run_seconds if run_seconds > 0 else 0.0
        print(
            f"run {run + 1}: files={len(files_with_sizes)} bytes={bytes_per_run} "
            f"time={run_seconds:.3f}s avg={run_avg_ms:.2f}ms/file throughput={run_mbps:.2f} MiB/s"
        )

    total_files = len(files_with_sizes) * args.repeat
    total_bytes = bytes_per_run * args.repeat
    avg_ms = (total_seconds / total_files) * 1000.0
    mbps = (total_bytes / (1024 * 1024)) / total_seconds if total_seconds > 0 else 0.0

    print(
        f"runs={args.repeat} total_files={total_files} total_bytes={total_bytes} "
        f"time={total_seconds:.3f}s avg={avg_ms:.2f}ms/file throughput={mbps:.2f} MiB/s"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
