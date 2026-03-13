#!/usr/bin/env python3
"""Benchmark decode worker/codec thread combinations on transcoded multiframe inputs."""

from __future__ import annotations

import argparse
import csv
import json
import os
import statistics
import sys
import tempfile
import time
from dataclasses import asdict, dataclass, replace
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[2]
BINDINGS_DIR = ROOT / "bindings" / "python"
BUILD_GLOBS = ("build*",)
NATIVE_PATTERNS = ("_dicomsdl*.pyd", "_dicomsdl*.so", "_dicomsdl*.dylib")
DEFAULT_BENCHMARK_KEYWORDS: tuple[str, ...] = (
    "ExplicitVRLittleEndian",
    "RLELossless",
    "JPEGBaseline8Bit",
    "JPEGExtended12Bit",
    "JPEGLossless",
    "JPEGLosslessNonHierarchical15",
    "JPEGLosslessSV1",
    "JPEGLSLossless",
    "JPEGLSNearLossless",
    "JPEG2000",
    "JPEG2000Lossless",
    "JPEG2000MC",
    "JPEG2000MCLossless",
    "HTJ2K",
    "HTJ2KLossless",
    "HTJ2KLosslessRPCL",
    "JPEGXL",
    "JPEGXLLossless",
)
UNCOMPRESSED_KEYWORDS = {
    "ImplicitVRLittleEndian",
    "ExplicitVRLittleEndian",
    "EncapsulatedUncompressedExplicitVRLittleEndian",
    "DeflatedExplicitVRLittleEndian",
    "ExplicitVRBigEndian",
    "Papyrus3ImplicitVRLittleEndian",
}
LOSSY_DEFAULT_OPTIONS: dict[str, dict[str, object]] = {
    "JPEGBaseline8Bit": {"type": "jpeg", "quality": 90},
    "JPEGExtended12Bit": {"type": "jpeg", "quality": 90},
    "JPEGLSNearLossless": {"type": "jpegls", "near_lossless_error": 2},
    "JPEG2000": {"type": "j2k", "target_psnr": 45},
    "JPEG2000MC": {"type": "j2k", "target_psnr": 45},
    "HTJ2K": {"type": "htj2k", "target_psnr": 50},
    "JPEGXL": {"type": "jpegxl", "distance": 1.0, "effort": 7},
}
LOSSLESS_DEFAULT_OPTIONS: dict[str, dict[str, object]] = {
    "JPEGLossless": {"type": "jpeg"},
    "JPEGLosslessNonHierarchical15": {"type": "jpeg"},
    "JPEGLosslessSV1": {"type": "jpeg"},
    "JPEGLSLossless": {"type": "jpegls", "near_lossless_error": 0},
    "JPEG2000Lossless": {"type": "j2k"},
    "JPEG2000MCLossless": {"type": "j2k"},
    "HTJ2KLossless": {"type": "htj2k"},
    "HTJ2KLosslessRPCL": {"type": "htj2k"},
    "JPEGXLLossless": {"type": "jpegxl", "distance": 0.0},
    "RLELossless": {"type": "rle"},
}
CODEC_SWEEP_WORKER_LABELS: tuple[str, ...] = ("1", "2")
CODEC_SWEEP_CODEC_LABELS: tuple[str, ...] = ("1", "2", "4", "8", "all")


def _find_native_module() -> Path:
    candidates: list[Path] = []
    for build_glob in BUILD_GLOBS:
        for build_dir in ROOT.glob(build_glob):
            if not build_dir.is_dir():
                continue
            for pattern in NATIVE_PATTERNS:
                candidates.extend(build_dir.rglob(pattern))
    if not candidates:
        raise RuntimeError(
            "Cannot find built native module '_dicomsdl'. "
            "Build the project first, for example with "
            "'cmake --build build-msyscheck --target _dicomsdl'."
        )
    candidates.sort(
        key=lambda path: (path.stat().st_mtime_ns, len(str(path))), reverse=True
    )
    return candidates[0]


def _find_build_root(native_module: Path) -> Path | None:
    for parent in native_module.parents:
        if parent == ROOT:
            return None
        if parent.name.startswith("build"):
            return parent
    return None


def _read_cmake_compiler_bin(build_root: Path | None) -> Path | None:
    if build_root is None:
        return None
    cache_path = build_root / "CMakeCache.txt"
    if not cache_path.exists():
        return None
    for line in cache_path.read_text(encoding="utf-8", errors="replace").splitlines():
        for prefix in ("CMAKE_CXX_COMPILER:FILEPATH=", "CMAKE_CXX_COMPILER:STRING="):
            if not line.startswith(prefix):
                continue
            compiler_path = Path(line[len(prefix) :].strip())
            if compiler_path.exists():
                return compiler_path.parent
    return None


def _configure_import_path(native_module: Path) -> None:
    desired_entries = [str(BINDINGS_DIR), str(native_module.parent)]
    for entry in reversed(desired_entries):
        if entry not in sys.path:
            sys.path.insert(0, entry)


def _configure_windows_dll_dirs(native_module: Path) -> None:
    if sys.platform != "win32":
        return
    add_dll_directory = getattr(os, "add_dll_directory", None)
    if add_dll_directory is None:
        return

    build_root = _find_build_root(native_module)
    candidate_dirs = [
        _read_cmake_compiler_bin(build_root),
        native_module.parent,
        build_root / "_deps" / "dicomsdl_openjpeg-build" / "bin" if build_root else None,
        build_root / "_deps" / "dicomsdl_charls-build" / "bin" if build_root else None,
        build_root / "_deps" / "dicomsdl_libjpeg_turbo-build" / "sharedlib"
        if build_root
        else None,
        build_root / "_deps" / "dicomsdl_libdeflate-build" if build_root else None,
    ]
    for path in candidate_dirs:
        if path is not None and path.exists():
            add_dll_directory(str(path))


native_module_path = _find_native_module()
_configure_windows_dll_dirs(native_module_path)
_configure_import_path(native_module_path)

import dicomsdl as dicom  # noqa: E402
import numpy as np  # noqa: E402


@dataclass
class ConvertedCase:
    source_path: str
    transfer_syntax: str
    output_path: str | None
    status: str
    detail: str
    frames: int
    rows: int
    cols: int
    samples_per_pixel: int
    bits_stored: int


@dataclass
class BenchmarkResult:
    matrix: str
    source_path: str
    transfer_syntax: str
    output_path: str
    frames: int
    worker_threads_requested: str
    worker_threads_actual: int
    codec_threads_requested: str
    codec_threads_actual: int
    inner_loops: int
    median_ms: float
    mean_ms: float


def _transfer_syntax_uses_codec_threads(
    transfer_syntax: str, htj2k_backend: str
) -> bool:
    if transfer_syntax.startswith("JPEG2000"):
        return True
    if transfer_syntax.startswith("JPEGXL"):
        return True
    if transfer_syntax.startswith("HTJ2K"):
        return htj2k_backend == "openjpeg"
    return False


def _resolve_input_paths(items: list[str]) -> list[Path]:
    paths: list[Path] = []
    for item in items:
        path = Path(item).expanduser()
        if not path.is_absolute():
            candidate_cwd = (Path.cwd() / path).resolve()
            candidate_root = (ROOT / path).resolve()
            if candidate_cwd.exists():
                path = candidate_cwd
            else:
                path = candidate_root
        else:
            path = path.resolve()
        paths.append(path)
    return paths


def _build_output_dir(cli_output_dir: str | None) -> Path:
    if cli_output_dir:
        return Path(cli_output_dir).expanduser().resolve()
    build_root = _find_build_root(native_module_path)
    if build_root is not None:
        return build_root / "multiframe_thread_bench"
    return ROOT / "multiframe_thread_bench"


def _supported_encode_keywords() -> set[str]:
    out: set[str] = set()
    for uid in dicom.transfer_syntax_uids_encode_supported():
        keyword = uid.keyword or uid.value
        if isinstance(keyword, str):
            out.add(keyword)
    return out


def _selected_transfer_syntax_uids(requested_keywords: list[str] | None) -> list[dicom.Uid]:
    if requested_keywords:
        keywords = requested_keywords
    else:
        supported = _supported_encode_keywords()
        keywords = [kw for kw in DEFAULT_BENCHMARK_KEYWORDS if kw in supported]

    out: list[dicom.Uid] = []
    for keyword in keywords:
        uid = dicom.lookup_uid(keyword)
        if uid is None:
            raise ValueError(f"Unknown transfer syntax keyword or UID: {keyword}")
        out.append(uid)
    return out


def _codec_options(keyword: str) -> dict[str, object] | None:
    if keyword in LOSSY_DEFAULT_OPTIONS:
        return dict(LOSSY_DEFAULT_OPTIONS[keyword])
    if keyword in LOSSLESS_DEFAULT_OPTIONS:
        return dict(LOSSLESS_DEFAULT_OPTIONS[keyword])
    return None


def _skip_reason(keyword: str, bits_stored: int, samples_per_pixel: int) -> str | None:
    if keyword in {"JPEG2000MC", "JPEG2000MCLossless"} and samples_per_pixel <= 1:
        return "multicomponent transfer syntax requires samples_per_pixel > 1"
    if keyword == "JPEGBaseline8Bit" and bits_stored > 8:
        return "baseline JPEG encode requires bits_stored <= 8"
    if keyword == "JPEGExtended12Bit" and bits_stored > 12:
        return "JPEG Extended encode requires bits_stored <= 12"
    return None


def _verify_decode_all_frames(df: dicom.DicomFile) -> None:
    plan = df.create_decode_plan(
        dicom.DecodeOptions(worker_threads=2, codec_threads=1)
    )
    reference = np.stack([df.to_array(frame=i, plan=plan) for i in range(plan.frames)], axis=0)
    out = np.empty(plan.shape(frame=-1), dtype=plan.dtype)
    returned = df.decode_into(out, frame=-1, plan=plan)
    if returned is not out:
        raise RuntimeError("decode_into(frame=-1) did not return the provided output array")
    if out.shape != reference.shape:
        raise RuntimeError(
            f"decode_all_frames shape mismatch: got {out.shape}, expected {reference.shape}"
        )
    if not np.array_equal(out, reference):
        raise RuntimeError("decode_all_frames output does not match per-frame decode")


def _is_unavailable_encoder_error(detail: str) -> bool:
    return "status=unsupported" in detail and "stage=plugin_lookup" in detail


def _convert_one(
    source_path: Path,
    uid: dicom.Uid,
    converted_root: Path,
) -> ConvertedCase:
    df = dicom.read_file(str(source_path))
    baseline_plan = df.create_decode_plan()
    keyword = uid.keyword or uid.value
    skip_reason = _skip_reason(
        keyword,
        baseline_plan.bits_stored,
        baseline_plan.samples_per_pixel,
    )
    if skip_reason is not None:
        return ConvertedCase(
            source_path=str(source_path),
            transfer_syntax=keyword,
            output_path=None,
            status="skipped",
            detail=skip_reason,
            frames=baseline_plan.frames,
            rows=baseline_plan.rows,
            cols=baseline_plan.cols,
            samples_per_pixel=baseline_plan.samples_per_pixel,
            bits_stored=baseline_plan.bits_stored,
        )

    input_dir = converted_root / source_path.stem
    input_dir.mkdir(parents=True, exist_ok=True)
    output_path = input_dir / f"{source_path.stem}_{keyword}.dcm"
    options = _codec_options(keyword)

    try:
        if options is None:
            df.set_transfer_syntax(uid)
        else:
            df.set_transfer_syntax(uid, options=options)
        df.write_file(str(output_path))
    except Exception as exc:
        detail = str(exc)
        status = "unavailable" if _is_unavailable_encoder_error(detail) else "convert_failed"
        return ConvertedCase(
            source_path=str(source_path),
            transfer_syntax=keyword,
            output_path=str(output_path),
            status=status,
            detail=detail,
            frames=baseline_plan.frames,
            rows=baseline_plan.rows,
            cols=baseline_plan.cols,
            samples_per_pixel=baseline_plan.samples_per_pixel,
            bits_stored=baseline_plan.bits_stored,
        )

    try:
        converted = dicom.read_file(str(output_path))
        _verify_decode_all_frames(converted)
        return ConvertedCase(
            source_path=str(source_path),
            transfer_syntax=keyword,
            output_path=str(output_path),
            status="verified",
            detail="decode_all_frames matches per-frame decode",
            frames=baseline_plan.frames,
            rows=baseline_plan.rows,
            cols=baseline_plan.cols,
            samples_per_pixel=baseline_plan.samples_per_pixel,
            bits_stored=baseline_plan.bits_stored,
        )
    except Exception as exc:
        return ConvertedCase(
            source_path=str(source_path),
            transfer_syntax=keyword,
            output_path=str(output_path),
            status="verify_failed",
            detail=str(exc),
            frames=baseline_plan.frames,
            rows=baseline_plan.rows,
            cols=baseline_plan.cols,
            samples_per_pixel=baseline_plan.samples_per_pixel,
            bits_stored=baseline_plan.bits_stored,
        )


def _parse_worker_threads(text: str, hardware_threads: int) -> list[tuple[str, int]]:
    out: list[tuple[str, int]] = []
    for token in (part.strip() for part in text.split(",")):
        if not token:
            continue
        if token.lower() == "all":
            out.append(("all", hardware_threads))
            continue
        value = int(token)
        if value <= 0:
            raise ValueError("worker thread requests must be positive integers or 'all'")
        out.append((str(value), value))
    if not out:
        raise ValueError("at least one worker thread request is required")
    return out


def _parse_codec_threads(text: str, hardware_threads: int) -> list[tuple[str, int]]:
    out: list[tuple[str, int]] = []
    for token in (part.strip() for part in text.split(",")):
        if not token:
            continue
        if token.lower() == "all":
            out.append(("all", hardware_threads))
            continue
        value = int(token)
        out.append((str(value), value))
    if not out:
        raise ValueError("at least one codec thread request is required")
    for _, value in out:
        if value < -1:
            raise ValueError("codec thread requests must be -1, 0, or positive")
    return out


def _build_codec_sweep_worker_cases(
    hardware_threads: int,
) -> list[tuple[str, int]]:
    out: list[tuple[str, int]] = []
    for label in CODEC_SWEEP_WORKER_LABELS:
        requested = hardware_threads if label == "all" else int(label)
        out.append((label, requested))
    return out


def _build_codec_sweep_codec_cases(
    hardware_threads: int,
) -> list[tuple[str, int]]:
    out: list[tuple[str, int]] = []
    for label in CODEC_SWEEP_CODEC_LABELS:
        requested = hardware_threads if label == "all" else int(label)
        out.append((label, requested))
    return out


def _benchmark_one(
    source_path: Path,
    converted_path: Path,
    transfer_syntax: str,
    worker_cases: list[tuple[str, int]],
    codec_thread_cases: list[tuple[str, int]],
    codec_sweep_worker_cases: list[tuple[str, int]],
    codec_sweep_codec_cases: list[tuple[str, int]],
    htj2k_backend: str,
    warmup: int,
    repeat: int,
    target_sample_ms: float,
    max_inner_loops: int,
) -> list[BenchmarkResult]:
    df = dicom.read_file(str(converted_path))
    reference_plan = df.create_decode_plan(
        dicom.DecodeOptions(worker_threads=1, codec_threads=1)
    )
    reference = np.empty(reference_plan.shape(frame=-1), dtype=reference_plan.dtype)
    df.decode_into(reference, frame=-1, plan=reference_plan)
    frames = int(reference_plan.frames)
    results: list[BenchmarkResult] = []
    uses_codec_threads = _transfer_syntax_uses_codec_threads(
        transfer_syntax, htj2k_backend
    )

    requests: list[tuple[str, str, int, str, int]] = []
    active_primary_codec_cases = (
        codec_thread_cases if uses_codec_threads else codec_thread_cases[:1]
    )
    for codec_label, codec_requested in active_primary_codec_cases:
        for worker_label, worker_requested in worker_cases:
            requests.append(
                ("primary", worker_label, worker_requested, codec_label, codec_requested)
            )
    if uses_codec_threads:
        for codec_label, codec_requested in codec_sweep_codec_cases:
            for worker_label, worker_requested in codec_sweep_worker_cases:
                requests.append(
                    (
                        "codec_sweep",
                        worker_label,
                        worker_requested,
                        codec_label,
                        codec_requested,
                    )
                )

    measured: dict[tuple[str, int, str, int], BenchmarkResult] = {}
    for matrix, worker_label, worker_requested, codec_label, codec_requested in requests:
        cache_key = (worker_label, worker_requested, codec_label, codec_requested)
        cached = measured.get(cache_key)
        if cached is not None:
            results.append(replace(cached, matrix=matrix))
            continue

        options = dicom.DecodeOptions(
            worker_threads=worker_requested,
            codec_threads=codec_requested,
        )
        plan = df.create_decode_plan(options)
        out = np.empty(plan.shape(frame=-1), dtype=plan.dtype)

        def run_once() -> None:
            df.decode_into(out, frame=-1, plan=plan)

        for _ in range(warmup):
            run_once()
        run_once()
        if not np.array_equal(out, reference):
            raise RuntimeError(
                "benchmark decode output mismatch for "
                f"{source_path.name} {transfer_syntax} "
                f"worker={worker_label} codec={codec_label}"
            )

        probe_start = time.perf_counter()
        run_once()
        probe_sec = max(time.perf_counter() - probe_start, 1e-9)
        inner_loops = max(
            1,
            min(max_inner_loops, int((target_sample_ms / 1000.0) / probe_sec)),
        )

        samples_ms: list[float] = []
        for _ in range(repeat):
            start = time.perf_counter()
            for _ in range(inner_loops):
                run_once()
            elapsed = time.perf_counter() - start
            samples_ms.append((elapsed / inner_loops) * 1000.0)

        actual_worker_count = min(worker_requested, frames)
        result = BenchmarkResult(
            matrix=matrix,
            source_path=str(source_path),
            transfer_syntax=transfer_syntax,
            output_path=str(converted_path),
            frames=frames,
            worker_threads_requested=worker_label,
            worker_threads_actual=actual_worker_count,
            codec_threads_requested=codec_label,
            codec_threads_actual=codec_requested,
            inner_loops=inner_loops,
            median_ms=statistics.median(samples_ms),
            mean_ms=statistics.fmean(samples_ms),
        )
        measured[cache_key] = result
        results.append(result)
    return results


def _format_outer_only_table(
    rows: list[BenchmarkResult],
    worker_order: list[str],
) -> str:
    by_ts = _group_outer_only_rows(rows)
    data_rows: list[list[str]] = []
    for transfer_syntax in DEFAULT_BENCHMARK_KEYWORDS:
        ts_rows = by_ts.get(transfer_syntax)
        if not ts_rows:
            continue
        data_rows.append(
            [
                f"`{transfer_syntax}`",
                *[
                    f"{ts_rows[worker].median_ms:.2f}" if worker in ts_rows else "-"
                    for worker in worker_order
                ],
            ]
        )
    return _render_markdown_table(["TS", *worker_order], data_rows)


def _group_outer_only_rows(
    rows: list[BenchmarkResult],
) -> dict[str, dict[str, BenchmarkResult]]:
    by_ts: dict[str, dict[str, BenchmarkResult]] = {}
    for row in rows:
        by_ts.setdefault(row.transfer_syntax, {})[row.worker_threads_requested] = row
    return by_ts


def _render_markdown_table(headers: list[str], data_rows: list[list[str]]) -> str:
    widths = [len(header) for header in headers]
    for row in data_rows:
        for index, cell in enumerate(row):
            widths[index] = max(widths[index], len(cell), 3)

    def format_row(values: list[str]) -> str:
        cells: list[str] = []
        for index, value in enumerate(values):
            if index == 0:
                cells.append(value.ljust(widths[index]))
            else:
                cells.append(value.rjust(widths[index]))
        return "| " + " | ".join(cells) + " |"

    separator_cells = []
    for index, width in enumerate(widths):
        if index == 0:
            separator_cells.append("-" * width)
        else:
            separator_cells.append("-" * (width - 1) + ":")

    lines = [
        format_row(headers),
        "| " + " | ".join(separator_cells) + " |",
    ]
    lines.extend(format_row(row) for row in data_rows)
    return "\n".join(lines)


def _format_codec_thread_table(
    rows: list[BenchmarkResult],
    worker_order: list[str],
    codec_order: list[str],
) -> str:
    by_ts: dict[str, dict[tuple[str, int], BenchmarkResult]] = {}
    for row in rows:
        by_ts.setdefault(row.transfer_syntax, {})[
            (row.worker_threads_requested, row.codec_threads_requested)
        ] = row

    value_headers = [
        f"{worker}/{codec_threads}"
        for codec_threads in codec_order
        for worker in worker_order
    ]
    data_rows: list[list[str]] = []
    for transfer_syntax in DEFAULT_BENCHMARK_KEYWORDS:
        ts_rows = by_ts.get(transfer_syntax)
        if not ts_rows:
            continue
        values: list[str] = []
        for codec_threads in codec_order:
            for worker in worker_order:
                result = ts_rows.get((worker, codec_threads))
                values.append(f"{result.median_ms:.2f}" if result is not None else "-")
        data_rows.append([f"`{transfer_syntax}`", *values])
    return _render_markdown_table(["TS", *value_headers], data_rows)


def _format_tables(
    rows: list[BenchmarkResult],
    worker_cases: list[tuple[str, int]],
    codec_thread_cases: list[tuple[str, int]],
    codec_sweep_worker_cases: list[tuple[str, int]],
    codec_sweep_codec_cases: list[tuple[str, int]],
    htj2k_backend: str,
) -> list[str]:
    worker_order = [label for label, _ in worker_cases]
    codec_order = [label for label, _ in codec_thread_cases]
    codec_sweep_worker_order = [label for label, _ in codec_sweep_worker_cases]
    codec_sweep_codec_order = [label for label, _ in codec_sweep_codec_cases]

    outer_only_rows = [
        row
        for row in rows
        if row.matrix == "primary"
        and not _transfer_syntax_uses_codec_threads(row.transfer_syntax, htj2k_backend)
    ]
    codec_thread_rows = [
        row
        for row in rows
        if row.matrix == "primary"
        and _transfer_syntax_uses_codec_threads(row.transfer_syntax, htj2k_backend)
    ]
    codec_sweep_rows = [
        row
        for row in rows
        if row.matrix == "codec_sweep"
        and _transfer_syntax_uses_codec_threads(row.transfer_syntax, htj2k_backend)
    ]

    sections: list[str] = []
    if outer_only_rows:
        sections.append("### Worker Thread")
        sections.append("")
        sections.append(_format_outer_only_table(outer_only_rows, worker_order))
    if codec_thread_rows:
        if sections:
            sections.append("")
        sections.append("### Worker Thread / Codec Thread")
        sections.append("")
        sections.append(_format_codec_thread_table(codec_thread_rows, worker_order, codec_order))
    if codec_sweep_rows:
        sections.append("")
        sections.append("### Codec Thread (Worker Thread = 1, 2)")
        sections.append("")
        sections.append(
            _format_codec_thread_table(
                codec_sweep_rows,
                codec_sweep_worker_order,
                codec_sweep_codec_order,
            )
        )
    return sections


def _write_report(
    output_dir: Path,
    inputs: list[Path],
    converted_cases: list[ConvertedCase],
    benchmark_results: list[BenchmarkResult],
    worker_cases: list[tuple[str, int]],
    codec_thread_cases: list[tuple[str, int]],
    codec_sweep_worker_cases: list[tuple[str, int]],
    codec_sweep_codec_cases: list[tuple[str, int]],
    hardware_threads: int,
    htj2k_backend: str,
) -> Path:
    payload = {
        "inputs": [str(path) for path in inputs],
        "hardware_threads": hardware_threads,
        "worker_cases": [label for label, _ in worker_cases],
        "codec_thread_cases": [label for label, _ in codec_thread_cases],
        "codec_sweep_worker_cases": [label for label, _ in codec_sweep_worker_cases],
        "codec_sweep_codec_cases": [label for label, _ in codec_sweep_codec_cases],
        "htj2k_backend": htj2k_backend,
        "converted_cases": [asdict(case) for case in converted_cases],
        "benchmark_results": [asdict(result) for result in benchmark_results],
    }
    (output_dir / "results.json").write_text(
        json.dumps(payload, indent=2),
        encoding="utf-8",
    )
    with (output_dir / "results.csv").open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=[
                "source_path",
                "transfer_syntax",
                "output_path",
                "frames",
                "matrix",
                "worker_threads_requested",
                "worker_threads_actual",
                "codec_threads_requested",
                "codec_threads_actual",
                "inner_loops",
                "median_ms",
                "mean_ms",
            ],
        )
        writer.writeheader()
        for row in benchmark_results:
            writer.writerow(asdict(row))
    (output_dir / "conversion_summary.json").write_text(
        json.dumps([asdict(case) for case in converted_cases], indent=2),
        encoding="utf-8",
    )

    lines = [
        "# Multiframe Thread Matrix Benchmark",
        "",
        f"- hardware threads: {hardware_threads}",
        "- value: median latency (ms)",
        f"- worker cases: {', '.join(label for label, _ in worker_cases)}",
        f"- codec thread cases: {', '.join(label for label, _ in codec_thread_cases)}",
        f"- codec sweep workers: {', '.join(label for label, _ in codec_sweep_worker_cases)}",
        f"- codec sweep threads: {', '.join(label for label, _ in codec_sweep_codec_cases)}",
        f"- HTJ2K backend: {htj2k_backend}",
        "",
    ]
    for input_path in inputs:
        lines.append(f"## {input_path.name}")
        lines.append("")
        verified = [
            row for row in benchmark_results if row.source_path == str(input_path)
        ]
        if not verified:
            lines.append("_no verified benchmark rows_")
            lines.append("")
            continue
        lines.extend(
            _format_tables(
                verified,
                worker_cases,
                codec_thread_cases,
                codec_sweep_worker_cases,
                codec_sweep_codec_cases,
                htj2k_backend,
            )
        )
        skipped = [
            case
            for case in converted_cases
            if case.source_path == str(input_path) and case.status != "verified"
        ]
        if skipped:
            lines.append("")
            lines.append("Skipped / unavailable:")
            for case in skipped:
                lines.append(
                    f"- `{case.transfer_syntax}`: `{case.status}` - {case.detail}"
                )
        lines.append("")
    report_path = output_dir / "report.md"
    report_path.write_text("\n".join(lines), encoding="utf-8")
    return report_path


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Transcode one or more multiframe DICOM inputs into benchmark transfer syntaxes, "
            "then measure decode worker/codec thread combinations."
        )
    )
    parser.add_argument(
        "inputs",
        nargs="+",
        help="One or more input DICOM file paths (absolute, cwd-relative, or repo-relative).",
    )
    parser.add_argument(
        "--output-dir",
        help=(
            "Directory to store report.md, results.json, results.csv, and conversion_summary.json. "
            "Defaults to <latest build>/multiframe_thread_bench."
        ),
    )
    parser.add_argument(
        "--keep-converted",
        action="store_true",
        help="Keep the transcoded temporary DICOM files under output-dir.",
    )
    parser.add_argument(
        "--transfer-syntax",
        action="append",
        dest="transfer_syntaxes",
        help=(
            "Limit benchmarked transfer syntaxes by keyword or UID value. "
            "May be specified multiple times."
        ),
    )
    parser.add_argument(
        "--worker-threads",
        default="1,2,4,8,all",
        help="Comma-separated worker thread requests (default: 1,2,4,8,all).",
    )
    parser.add_argument(
        "--codec-threads",
        default="1,2",
        help="Comma-separated codec thread requests (default: 1,2).",
    )
    parser.add_argument(
        "--warmup",
        type=int,
        default=1,
        help="Warmup iterations per benchmark cell (default: 1).",
    )
    parser.add_argument(
        "--repeat",
        type=int,
        default=4,
        help="Measured samples per benchmark cell (default: 4).",
    )
    parser.add_argument(
        "--target-sample-ms",
        type=float,
        default=150.0,
        help="Adaptive timing target per measured sample in milliseconds (default: 150).",
    )
    parser.add_argument(
        "--max-inner-loops",
        type=int,
        default=128,
        help="Maximum adaptive inner loops for very fast decodes (default: 128).",
    )
    parser.add_argument(
        "--htj2k-backend",
        choices=("auto", "openjph", "openjpeg"),
        default="openjph",
        help="HTJ2K decoder backend preference before any pixel runtime use (default: openjph).",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)

    if args.warmup < 0 or args.repeat <= 0:
        raise ValueError("warmup must be >= 0 and repeat must be > 0")
    if args.target_sample_ms <= 0.0 or args.max_inner_loops <= 0:
        raise ValueError("target-sample-ms and max-inner-loops must be positive")

    if args.htj2k_backend == "openjph":
        dicom.use_openjph_for_htj2k_decoding()
    elif args.htj2k_backend == "openjpeg":
        dicom.use_openjpeg_for_htj2k_decoding()
    else:
        dicom.set_htj2k_decoder_backend("auto")

    inputs = _resolve_input_paths(args.inputs)
    output_dir = _build_output_dir(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    hardware_threads = os.cpu_count() or 1
    worker_cases = _parse_worker_threads(args.worker_threads, hardware_threads)
    codec_thread_cases = _parse_codec_threads(args.codec_threads, hardware_threads)
    codec_sweep_worker_cases = _build_codec_sweep_worker_cases(hardware_threads)
    codec_sweep_codec_cases = _build_codec_sweep_codec_cases(hardware_threads)
    transfer_syntax_uids = _selected_transfer_syntax_uids(args.transfer_syntaxes)

    temp_ctx: tempfile.TemporaryDirectory[str] | None = None
    if args.keep_converted:
        converted_dir = output_dir / f"converted_{int(time.time())}"
        converted_dir.mkdir(parents=True, exist_ok=True)
    else:
        temp_ctx = tempfile.TemporaryDirectory(
            prefix="converted_",
            dir=output_dir,
            ignore_cleanup_errors=True,
        )
        converted_dir = Path(temp_ctx.name)

    converted_cases: list[ConvertedCase] = []
    benchmark_results: list[BenchmarkResult] = []

    try:
        for input_path in inputs:
            if not input_path.exists():
                converted_cases.append(
                    ConvertedCase(
                        source_path=str(input_path),
                        transfer_syntax="-",
                        output_path=None,
                        status="missing_input",
                        detail="input file does not exist",
                        frames=0,
                        rows=0,
                        cols=0,
                        samples_per_pixel=0,
                        bits_stored=0,
                    )
                )
                print(f"[missing] {input_path}")
                continue

            print(f"[input] {input_path}")
            for uid in transfer_syntax_uids:
                keyword = uid.keyword or uid.value
                case = _convert_one(input_path, uid, converted_dir)
                converted_cases.append(case)
                print(f"  [{case.status:13}] {keyword}: {case.detail}")
                if case.status != "verified" or case.output_path is None:
                    continue
                bench_rows = _benchmark_one(
                    input_path,
                    Path(case.output_path),
                    case.transfer_syntax,
                    worker_cases,
                    codec_thread_cases,
                    codec_sweep_worker_cases,
                    codec_sweep_codec_cases,
                    args.htj2k_backend,
                    warmup=args.warmup,
                    repeat=args.repeat,
                    target_sample_ms=args.target_sample_ms,
                    max_inner_loops=args.max_inner_loops,
                )
                benchmark_results.extend(bench_rows)
                print(
                    f"    [bench ok] {case.transfer_syntax}: {len(bench_rows)} cells"
                )

        report_path = _write_report(
            output_dir,
            inputs,
            converted_cases,
            benchmark_results,
            worker_cases,
            codec_thread_cases,
            codec_sweep_worker_cases,
            codec_sweep_codec_cases,
            hardware_threads,
            args.htj2k_backend,
        )
        print(f"[report] {report_path}")
        print(report_path.read_text(encoding="utf-8"))
    finally:
        if temp_ctx is not None:
            temp_ctx.cleanup()

    failed = [
        case
        for case in converted_cases
        if case.status in {"missing_input", "convert_failed", "verify_failed"}
    ]
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
