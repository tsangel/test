#!/usr/bin/env python3
"""Benchmark DICOM JSON read/write paths for a directory tree of DICOM files.

This benchmark recursively scans a base directory, keeps files that DicomSDL can
read as DICOM, precomputes DICOM JSON with BulkDataURI output, then compares:

- dicomsdl.read_bytes(..., copy=False, load_until=(7FE0,0000))
- DicomFile.write_json(..., bulk_data="uri")
- dicomsdl.read_json(...)
- python json.loads(...)

Example:
    python benchmarks/python/benchmark_json_read_write.py ../sample/nema/WG04/IMAGES/J2KR
"""

from __future__ import annotations

import argparse
import json
import os
import statistics
import sys
import sysconfig
import time
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any, Callable


_THIS_DIR = Path(__file__).resolve().parent
_REPO_ROOT = _THIS_DIR.parents[1]
_EXAMPLES_PY = _REPO_ROOT / "examples" / "python" / "itk_vtk"
if str(_EXAMPLES_PY) not in sys.path:
    sys.path.insert(0, str(_EXAMPLES_PY))

from _repo_dicomsdl_import import configure_repo_dicomsdl_import


def _current_extension_suffix() -> str:
    return (sysconfig.get_config_var("EXT_SUFFIX") or "").strip()


def _candidate_priority(path: Path) -> tuple[bool, int, int]:
    current_suffix = _current_extension_suffix()
    suffix_matches = bool(current_suffix) and path.name.endswith(current_suffix)
    return (suffix_matches, path.stat().st_mtime_ns, len(str(path)))


def _find_native_module(repo_root: Path) -> Path | None:
    explicit = os.environ.get("DICOMSDL_NATIVE_MODULE_PATH", "").strip()
    if explicit:
        candidate = Path(explicit).expanduser().resolve()
        if candidate.is_file():
            return candidate

    candidates: list[Path] = []
    for build_dir in repo_root.glob("build*"):
        if not build_dir.is_dir():
            continue
        candidates.extend(build_dir.rglob("_dicomsdl*.pyd"))
        candidates.extend(build_dir.rglob("_dicomsdl*.so"))
        candidates.extend(build_dir.rglob("_dicomsdl*.dylib"))
    if not candidates:
        return None
    candidates.sort(key=_candidate_priority, reverse=True)
    return candidates[0]


def _configure_windows_dll_dirs(native_module: Path) -> None:
    if sys.platform != "win32":
        return
    add_dll_directory = getattr(os, "add_dll_directory", None)
    if add_dll_directory is None:
        return
    candidate_dirs = [native_module.parent]
    for path in candidate_dirs:
        if path.exists():
            add_dll_directory(str(path))


native_module = _find_native_module(_REPO_ROOT)
if native_module is not None:
    os.environ["DICOMSDL_NATIVE_MODULE_PATH"] = str(native_module)
    _configure_windows_dll_dirs(native_module)

configure_repo_dicomsdl_import()

import dicomsdl as dicom


PIXEL_STOP_TAG = dicom.Tag(0x7FE0, 0x0000)
DEFAULT_BULK_URI_TEMPLATE = "/bulk/{tag}"
DEFAULT_PIXEL_URI_TEMPLATE = "/frames"


@dataclass(slots=True)
class PreparedCase:
    path: str
    dicom_bytes: bytes
    json_bytes: bytes
    write_df: Any
    bulk_ref_count: int
    bulk_part_count: int


@dataclass(slots=True)
class BenchmarkRow:
    mode: str
    runs_sec: list[float]
    total_files: int
    total_dicom_bytes: int
    total_json_bytes: int
    sink: int


def _iter_candidate_files(root: Path) -> list[Path]:
    return sorted(
        path for path in root.rglob("*")
        if path.is_file() and dicom.is_dicom_file(path)
    )


def _prepare_cases(
    root: Path,
    *,
    bulk_uri_template: str,
    pixel_uri_template: str,
    verbose: bool,
) -> tuple[list[PreparedCase], list[str]]:
    cases: list[PreparedCase] = []
    skipped: list[str] = []

    for path in _iter_candidate_files(root):
        raw = path.read_bytes()
        try:
            dicom.read_bytes(raw, name=str(path), copy=False, load_until=PIXEL_STOP_TAG)
        except Exception as exc:  # noqa: BLE001 - benchmark input filter
            skipped.append(f"{path}: {exc}")
            continue

        write_df = dicom.read_bytes(raw, name=str(path), copy=False)
        json_text, bulk_parts = write_df.write_json(
            bulk_data="uri",
            bulk_data_uri_template=bulk_uri_template,
            pixel_data_uri_template=pixel_uri_template,
        )
        json_bytes = json_text.encode("utf-8")
        read_items = dicom.read_json(json_bytes)
        bulk_ref_count = sum(len(refs) for _, refs in read_items)

        cases.append(
            PreparedCase(
                path=str(path),
                dicom_bytes=raw,
                json_bytes=json_bytes,
                write_df=write_df,
                bulk_ref_count=bulk_ref_count,
                bulk_part_count=len(bulk_parts),
            )
        )
        if verbose:
            print(
                f"[prepare] {path} dicom={len(raw)}B json={len(json_bytes)}B "
                f"bulk_parts={len(bulk_parts)} bulk_refs={bulk_ref_count}"
            )

    return cases, skipped


def _bench_mode(
    mode: str,
    cases: list[PreparedCase],
    func: Callable[[PreparedCase], int],
    *,
    warmup: int,
    repeat: int,
) -> BenchmarkRow:
    for _ in range(warmup):
        for case in cases:
            _ = func(case)

    runs: list[float] = []
    sink = 0
    for _ in range(repeat):
        start = time.perf_counter()
        run_sink = 0
        for case in cases:
            run_sink += func(case)
        elapsed = time.perf_counter() - start
        runs.append(elapsed)
        sink = run_sink

    return BenchmarkRow(
        mode=mode,
        runs_sec=runs,
        total_files=len(cases),
        total_dicom_bytes=sum(len(case.dicom_bytes) for case in cases),
        total_json_bytes=sum(len(case.json_bytes) for case in cases),
        sink=sink,
    )


def _summarize_row(row: BenchmarkRow) -> str:
    mean_s = statistics.mean(row.runs_sec)
    median_s = statistics.median(row.runs_sec)
    min_s = min(row.runs_sec)
    max_s = max(row.runs_sec)
    avg_ms_per_file = (mean_s / row.total_files) * 1000.0 if row.total_files else 0.0
    return (
        f"{row.mode:<20} mean={mean_s*1000.0:8.3f} ms  "
        f"median={median_s*1000.0:8.3f} ms  min={min_s*1000.0:8.3f} ms  "
        f"max={max_s*1000.0:8.3f} ms  avg={avg_ms_per_file:7.3f} ms/file"
    )


def _write_report(
    path: Path,
    *,
    root: Path,
    bulk_uri_template: str,
    pixel_uri_template: str,
    cases: list[PreparedCase],
    skipped: list[str],
    rows: list[BenchmarkRow],
    warmup: int,
    repeat: int,
) -> None:
    report = {
        "root": str(root),
        "bulk_uri_template": bulk_uri_template,
        "pixel_uri_template": pixel_uri_template,
        "warmup": warmup,
        "repeat": repeat,
        "prepared_files": len(cases),
        "prepared_dicom_bytes": sum(len(case.dicom_bytes) for case in cases),
        "prepared_json_bytes": sum(len(case.json_bytes) for case in cases),
        "bulk_part_count": sum(case.bulk_part_count for case in cases),
        "bulk_ref_count": sum(case.bulk_ref_count for case in cases),
        "cases": [
            {
                "path": case.path,
                "dicom_bytes": len(case.dicom_bytes),
                "json_bytes": len(case.json_bytes),
                "bulk_part_count": case.bulk_part_count,
                "bulk_ref_count": case.bulk_ref_count,
            }
            for case in cases
        ],
        "skipped": skipped,
        "rows": [asdict(row) for row in rows],
    }
    path.write_text(json.dumps(report, indent=2), encoding="utf-8")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Benchmark DicomSDL JSON read/write against read_bytes(copy=False) and python json.loads "
            "for every readable DICOM file under a directory tree."
        )
    )
    parser.add_argument("root", type=Path, help="Root directory to scan recursively")
    parser.add_argument("--warmup", type=int, default=1, help="Warmup passes per mode")
    parser.add_argument("--repeat", type=int, default=5, help="Measured passes per mode")
    parser.add_argument(
        "--bulk-uri-template",
        default=DEFAULT_BULK_URI_TEMPLATE,
        help=f"Template for non-PixelData BulkDataURI values (default: {DEFAULT_BULK_URI_TEMPLATE})",
    )
    parser.add_argument(
        "--pixel-uri-template",
        default=DEFAULT_PIXEL_URI_TEMPLATE,
        help=f"Template override for PixelData BulkDataURI values (default: {DEFAULT_PIXEL_URI_TEMPLATE})",
    )
    parser.add_argument("--json", dest="json_path", type=Path, help="Write benchmark report to JSON")
    parser.add_argument("--verbose", action="store_true", help="Print prepared file details")
    args = parser.parse_args(argv)

    root = args.root.resolve()
    if not root.exists():
        print(f"error: root does not exist: {root}", file=sys.stderr)
        return 2
    if not root.is_dir():
        print(f"error: root is not a directory: {root}", file=sys.stderr)
        return 2

    print(f"Root: {root}")
    print(f"Warmup: {args.warmup}")
    print(f"Repeat: {args.repeat}")
    print(f"bulk_data_uri_template: {args.bulk_uri_template}")
    print(f"pixel_data_uri_template: {args.pixel_uri_template}")
    print()

    cases, skipped = _prepare_cases(
        root,
        bulk_uri_template=args.bulk_uri_template,
        pixel_uri_template=args.pixel_uri_template,
        verbose=args.verbose,
    )
    if not cases:
        print("error: no readable DICOM files found under the given root", file=sys.stderr)
        if skipped:
            print("skipped:", file=sys.stderr)
            for line in skipped[:10]:
                print(f"  {line}", file=sys.stderr)
        return 2

    print(f"Prepared files: {len(cases)}")
    print(f"Skipped files:  {len(skipped)}")
    print(f"Total DICOM:    {sum(len(case.dicom_bytes) for case in cases):,} bytes")
    print(f"Total JSON:     {sum(len(case.json_bytes) for case in cases):,} bytes")
    print(f"Bulk parts:     {sum(case.bulk_part_count for case in cases)}")
    print(f"Bulk refs:      {sum(case.bulk_ref_count for case in cases)}")
    print()

    rows = [
        _bench_mode(
            "read_bytes_copy_false",
            cases,
            lambda case: len(
                dicom.read_bytes(
                    case.dicom_bytes,
                    name=case.path,
                    copy=False,
                    load_until=PIXEL_STOP_TAG,
                )
            ),
            warmup=args.warmup,
            repeat=args.repeat,
        ),
        _bench_mode(
            "write_json_uri",
            cases,
            lambda case: len(
                case.write_df.write_json(
                    bulk_data="uri",
                    bulk_data_uri_template=args.bulk_uri_template,
                    pixel_data_uri_template=args.pixel_uri_template,
                )[0]
            ),
            warmup=args.warmup,
            repeat=args.repeat,
        ),
        _bench_mode(
            "read_json",
            cases,
            lambda case: len(dicom.read_json(case.json_bytes)),
            warmup=args.warmup,
            repeat=args.repeat,
        ),
        _bench_mode(
            "python_json_loads",
            cases,
            lambda case: len(json.loads(case.json_bytes)),
            warmup=args.warmup,
            repeat=args.repeat,
        ),
    ]

    for row in rows:
        print(_summarize_row(row))

    if args.json_path is not None:
        _write_report(
            args.json_path,
            root=root,
            bulk_uri_template=args.bulk_uri_template,
            pixel_uri_template=args.pixel_uri_template,
            cases=cases,
            skipped=skipped,
            rows=rows,
            warmup=args.warmup,
            repeat=args.repeat,
        )
        print()
        print(f"Wrote JSON report: {args.json_path}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
