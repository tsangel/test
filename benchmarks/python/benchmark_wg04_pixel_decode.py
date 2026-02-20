#!/usr/bin/env python3
"""Benchmark WG04 pixel decode throughput by codec and backend."""

from __future__ import annotations

import argparse
import json
import os
import statistics
import sys
import time
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any


_DEFAULT_WG04_IMAGES_ROOT = Path("/Users/tsangel/workspace.dev/sample/nema/WG04/IMAGES")
_WG04_IMAGES_ENV = "DICOMSDL_WG04_IMAGES_BASE"

_HTJ2K_CODECS: tuple[str, ...] = ("htj2kll", "htj2kly")
_CODEC_ORDER: tuple[str, ...] = (
    "REF",
    "RLE",
    "J2KR",
    "J2KI",
    "htj2kll",
    "htj2kly",
    "JLSL",
    "JLSN",
    "JPLL",
    "JPLY",
)

_CODEC_LABEL: dict[str, str] = {
    "REF": "Raw/Reference",
    "RLE": "RLE Lossless",
    "J2KR": "JPEG2000 Lossless",
    "J2KI": "JPEG2000 Lossy",
    "htj2kll": "HTJ2K Lossless",
    "htj2kly": "HTJ2K Lossy",
    "JLSL": "JPEG-LS Lossless",
    "JLSN": "JPEG-LS Near-lossless",
    "JPLL": "JPEG Lossless",
    "JPLY": "JPEG Lossy",
}

_BACKEND_ORDER: tuple[str, ...] = ("dicomsdl", "pydicom")
_DICOMSDL_MODE_ORDER: tuple[str, ...] = (
    "to_array",
    "decode_into",
    "to_array_view",
    "to_array_view_copyto",
)
_PYDICOM_MODE_ORDER: tuple[str, ...] = ("pixel_array", "reuse_output")
_RAW_BEST_CODEC = "REF"
_RAW_BEST_DICOMSDL_MODE = "to_array_view_copyto"
_RAW_BEST_PYDICOM_MODE = "reuse_output"
_J2K_CODECS: tuple[str, ...] = ("J2KR", "J2KI")


def _is_htj2k_codec(codec: str) -> bool:
    normalized = codec.removesuffix("_MT")
    return normalized.lower().startswith("htj2k")


@dataclass
class CodecStats:
    backend: str
    codec: str
    mode: str
    label: str
    files: int
    warmup: int
    repeat: int
    decodes: int
    total_pixels: int
    total_bytes: int
    elapsed_s: float
    mean_run_s: float
    median_run_s: float
    min_run_s: float
    max_run_s: float
    ms_per_decode: float
    mpix_s: float
    mib_s: float


@dataclass
class PydicomReuseContext:
    outputs: list[Any]
    use_numpy_handler: list[bool]
    get_pixeldata: Any
    reshape_pixel_array: Any
    copyto: Any


@dataclass
class DicomsdlViewCopytoContext:
    outputs: list[Any]
    use_view: list[bool]
    copyto: Any


def _resolve_root(cli_root: Path | None) -> Path:
    if cli_root is not None:
        return cli_root.expanduser().resolve()
    override = os.environ.get(_WG04_IMAGES_ENV)
    if override:
        return Path(override).expanduser().resolve()
    return _DEFAULT_WG04_IMAGES_ROOT


def _codec_paths(root: Path, codec: str) -> list[Path]:
    directory = root / codec
    if not directory.exists():
        raise FileNotFoundError(f"missing WG04 codec directory: {directory}")
    files = sorted(path for path in directory.iterdir() if path.is_file())
    if not files:
        raise RuntimeError(f"no files found in WG04 codec directory: {directory}")
    return files


def _case_id_from_filename(name: str) -> str:
    # WG04 files follow "<case>_<codec>" naming (e.g., CT1_J2KR, CT1_UNC).
    # Use the case token to pair compressed files with REF for compression ratio.
    if "_" not in name:
        return name
    return name.rsplit("_", 1)[0]


def _build_compression_ratio_maps(
    root: Path, codecs: list[str]
) -> tuple[dict[str, float], dict[str, int]]:
    ref_paths = _codec_paths(root, _RAW_BEST_CODEC)
    ref_sizes = {_case_id_from_filename(path.name): path.stat().st_size for path in ref_paths}

    ratio_by_codec: dict[str, float] = {}
    pairs_by_codec: dict[str, int] = {}

    for codec in codecs:
        codec_paths = _codec_paths(root, codec)
        codec_sizes = {_case_id_from_filename(path.name): path.stat().st_size for path in codec_paths}
        common_cases = sorted(set(ref_sizes) & set(codec_sizes))
        if not common_cases:
            continue

        ratios: list[float] = []
        for case_id in common_cases:
            codec_size = codec_sizes[case_id]
            if codec_size <= 0:
                continue
            ratios.append(ref_sizes[case_id] / codec_size)

        if ratios:
            ratio_by_codec[codec] = statistics.mean(ratios)
            pairs_by_codec[codec] = len(ratios)

    return ratio_by_codec, pairs_by_codec


def _import_backend(backend: str) -> Any:
    if backend == "dicomsdl":
        import dicomsdl as mod

        return mod
    if backend == "pydicom":
        import pydicom as mod

        return mod
    raise ValueError(f"unsupported backend: {backend}")


def _preload_inputs(paths: list[Path], backend: str, module: Any) -> list[tuple[Path, Any]]:
    inputs: list[tuple[Path, Any]] = []
    if backend == "dicomsdl":
        for path in paths:
            inputs.append((path, module.read_file(str(path))))
        return inputs

    if backend == "pydicom":
        for path in paths:
            inputs.append((path, module.dcmread(str(path))))
        return inputs

    raise ValueError(f"unsupported backend: {backend}")


def _decode_pydicom_pixel_array(ds: Any) -> Any:
    # Force pydicom to re-run pixel conversion/decompression each decode call.
    if hasattr(ds, "_pixel_id"):
        ds._pixel_id = {}
    return ds.pixel_array


def _prepare_reuse_outputs(
    inputs: list[tuple[Path, Any]],
    backend: str,
    *,
    scaled: bool,
    dicomsdl_htj2k_decoder: str,
) -> list[Any] | None:
    if backend != "dicomsdl":
        return None
    try:
        import numpy as np  # noqa: F401
    except Exception as exc:  # pragma: no cover
        raise RuntimeError("--reuse-output requires NumPy") from exc

    outputs: list[Any] = []
    for _, ds in inputs:
        probe = ds.to_array(frame=-1, scaled=scaled, htj2k_decoder=dicomsdl_htj2k_decoder)
        outputs.append(np.empty_like(probe))
    return outputs


def _prepare_pydicom_reuse_context(inputs: list[tuple[Path, Any]]) -> PydicomReuseContext:
    try:
        import numpy as np
    except Exception as exc:  # pragma: no cover
        raise RuntimeError("--reuse-output-pydicom requires NumPy") from exc

    from pydicom.pixel_data_handlers import numpy_handler
    from pydicom.pixel_data_handlers import util as pydicom_util

    outputs: list[Any] = []
    use_numpy_handler: list[bool] = []

    for _, ds in inputs:
        tsuid = getattr(getattr(ds, "file_meta", None), "TransferSyntaxUID", None)
        is_compressed = bool(getattr(tsuid, "is_compressed", False))

        if not is_compressed:
            flat = numpy_handler.get_pixeldata(ds, read_only=True)
            shaped = pydicom_util.reshape_pixel_array(ds, flat)
            outputs.append(np.empty_like(shaped))
            use_numpy_handler.append(True)
            continue

        probe = ds.pixel_array
        outputs.append(np.empty_like(probe))
        use_numpy_handler.append(False)

    return PydicomReuseContext(
        outputs=outputs,
        use_numpy_handler=use_numpy_handler,
        get_pixeldata=numpy_handler.get_pixeldata,
        reshape_pixel_array=pydicom_util.reshape_pixel_array,
        copyto=np.copyto,
    )


def _prepare_dicomsdl_view_copyto_context(
    inputs: list[tuple[Path, Any]], *, scaled: bool, dicomsdl_htj2k_decoder: str
) -> DicomsdlViewCopytoContext:
    try:
        import numpy as np
    except Exception as exc:  # pragma: no cover
        raise RuntimeError("--dicomsdl-mode=to_array_view_copyto requires NumPy") from exc

    outputs: list[Any] = []
    use_view: list[bool] = []

    for _, ds in inputs:
        try:
            view = ds.to_array_view(frame=-1)
            outputs.append(np.empty_like(view))
            use_view.append(True)
        except Exception:
            probe = ds.to_array(frame=-1, scaled=scaled, htj2k_decoder=dicomsdl_htj2k_decoder)
            outputs.append(np.empty_like(probe))
            use_view.append(False)

    return DicomsdlViewCopytoContext(outputs=outputs, use_view=use_view, copyto=np.copyto)


def _benchmark_codec(
    backend: str,
    codec: str,
    inputs: list[tuple[Path, Any]],
    *,
    warmup: int,
    repeat: int,
    scaled: bool,
    dicomsdl_mode: str,
    dicomsdl_htj2k_decoder: str,
    pydicom_mode: str,
    verbose: bool,
    decoder_threads: int = -1,
) -> CodecStats:
    reusable_outputs: list[Any] | None = None
    dicomsdl_view_copyto: DicomsdlViewCopytoContext | None = None
    pydicom_reuse: PydicomReuseContext | None = None
    if backend == "dicomsdl" and dicomsdl_mode == "decode_into":
        reusable_outputs = _prepare_reuse_outputs(
            inputs,
            backend,
            scaled=scaled,
            dicomsdl_htj2k_decoder=dicomsdl_htj2k_decoder,
        )
    if backend == "dicomsdl" and dicomsdl_mode == "to_array_view_copyto":
        dicomsdl_view_copyto = _prepare_dicomsdl_view_copyto_context(
            inputs, scaled=scaled, dicomsdl_htj2k_decoder=dicomsdl_htj2k_decoder
        )
        fallback_count = sum(1 for enabled in dicomsdl_view_copyto.use_view if not enabled)
        if fallback_count > 0:
            print(
                f"note: dicomsdl to_array_view_copyto fallback to decode_into on {fallback_count} "
                f"file(s) in codec {codec}"
            )
    if backend == "pydicom" and pydicom_mode == "reuse_output":
        pydicom_reuse = _prepare_pydicom_reuse_context(inputs)
        fallback_count = sum(1 for enabled in pydicom_reuse.use_numpy_handler if not enabled)
        if fallback_count > 0:
            print(
                f"note: pydicom reuse-output fallback to pixel_array on {fallback_count} "
                f"compressed file(s) in codec {codec}"
            )

    for _ in range(warmup):
        for index, (_, ds) in enumerate(inputs):
            if backend == "dicomsdl":
                if dicomsdl_mode == "decode_into" and reusable_outputs is not None:
                    _ = ds.decode_into(
                        reusable_outputs[index],
                        frame=-1,
                        scaled=scaled,
                        threads=decoder_threads,
                        htj2k_decoder=dicomsdl_htj2k_decoder,
                    )
                elif dicomsdl_mode == "to_array_view_copyto" and dicomsdl_view_copyto is not None:
                    if dicomsdl_view_copyto.use_view[index]:
                        dicomsdl_view_copyto.copyto(
                            dicomsdl_view_copyto.outputs[index], ds.to_array_view(frame=-1)
                        )
                    else:
                        _ = ds.decode_into(
                            dicomsdl_view_copyto.outputs[index],
                            frame=-1,
                            scaled=scaled,
                            threads=decoder_threads,
                            htj2k_decoder=dicomsdl_htj2k_decoder,
                        )
                elif dicomsdl_mode == "to_array_view":
                    _ = ds.to_array_view(frame=-1)
                else:
                    _ = ds.to_array(
                        frame=-1, scaled=scaled, htj2k_decoder=dicomsdl_htj2k_decoder
                    )
            elif pydicom_reuse is not None:
                if pydicom_reuse.use_numpy_handler[index]:
                    flat = pydicom_reuse.get_pixeldata(ds, read_only=True)
                    shaped = pydicom_reuse.reshape_pixel_array(ds, flat)
                    pydicom_reuse.copyto(pydicom_reuse.outputs[index], shaped)
                else:
                    pydicom_reuse.copyto(pydicom_reuse.outputs[index], _decode_pydicom_pixel_array(ds))
            else:
                _ = _decode_pydicom_pixel_array(ds)

    run_times: list[float] = []
    total_pixels = 0
    total_bytes = 0

    for run_index in range(repeat):
        run_pixels = 0
        run_bytes = 0
        t0 = time.perf_counter()
        for index, (path, ds) in enumerate(inputs):
            if backend == "dicomsdl":
                if dicomsdl_mode == "decode_into" and reusable_outputs is not None:
                    arr = ds.decode_into(
                        reusable_outputs[index],
                        frame=-1,
                        scaled=scaled,
                        threads=decoder_threads,
                        htj2k_decoder=dicomsdl_htj2k_decoder,
                    )
                elif dicomsdl_mode == "to_array_view_copyto" and dicomsdl_view_copyto is not None:
                    if dicomsdl_view_copyto.use_view[index]:
                        dicomsdl_view_copyto.copyto(
                            dicomsdl_view_copyto.outputs[index], ds.to_array_view(frame=-1)
                        )
                    else:
                        _ = ds.decode_into(
                            dicomsdl_view_copyto.outputs[index],
                            frame=-1,
                            scaled=scaled,
                            threads=decoder_threads,
                            htj2k_decoder=dicomsdl_htj2k_decoder,
                        )
                    arr = dicomsdl_view_copyto.outputs[index]
                elif dicomsdl_mode == "to_array_view":
                    arr = ds.to_array_view(frame=-1)
                else:
                    arr = ds.to_array(
                        frame=-1, scaled=scaled, htj2k_decoder=dicomsdl_htj2k_decoder
                    )
            elif pydicom_reuse is not None:
                if pydicom_reuse.use_numpy_handler[index]:
                    flat = pydicom_reuse.get_pixeldata(ds, read_only=True)
                    shaped = pydicom_reuse.reshape_pixel_array(ds, flat)
                    pydicom_reuse.copyto(pydicom_reuse.outputs[index], shaped)
                    arr = pydicom_reuse.outputs[index]
                else:
                    pydicom_reuse.copyto(pydicom_reuse.outputs[index], _decode_pydicom_pixel_array(ds))
                    arr = pydicom_reuse.outputs[index]
            else:
                arr = _decode_pydicom_pixel_array(ds)
            run_pixels += int(arr.size)
            run_bytes += int(arr.nbytes)
            if verbose:
                print(
                    f"{backend} {codec} run={run_index + 1}/{repeat} file={path.name} "
                    f"shape={arr.shape} dtype={arr.dtype}"
                )
        elapsed = time.perf_counter() - t0
        run_times.append(elapsed)
        total_pixels += run_pixels
        total_bytes += run_bytes

    decodes = len(inputs) * repeat
    total_elapsed = sum(run_times)
    ms_per_decode = (total_elapsed * 1000.0 / decodes) if decodes else 0.0
    mpix_s = (total_pixels / 1_000_000.0 / total_elapsed) if total_elapsed > 0.0 else 0.0
    mib_s = (total_bytes / (1024.0 * 1024.0) / total_elapsed) if total_elapsed > 0.0 else 0.0

    return CodecStats(
        backend=backend,
        codec=codec,
        mode=(dicomsdl_mode if backend == "dicomsdl" else pydicom_mode),
        label=_CODEC_LABEL.get(codec, codec),
        files=len(inputs),
        warmup=warmup,
        repeat=repeat,
        decodes=decodes,
        total_pixels=total_pixels,
        total_bytes=total_bytes,
        elapsed_s=total_elapsed,
        mean_run_s=statistics.mean(run_times),
        median_run_s=statistics.median(run_times),
        min_run_s=min(run_times),
        max_run_s=max(run_times),
        ms_per_decode=ms_per_decode,
        mpix_s=mpix_s,
        mib_s=mib_s,
    )


def _print_backend_report(
    root: Path,
    backend: str,
    stats: list[CodecStats],
    *,
    scaled: bool,
    mode: str,
) -> None:
    print(f"\n== {backend} ==")
    print(f"WG04 root: {root}")
    print(f"scaled output: {scaled}")
    mode_set = sorted({row.mode for row in stats})
    if len(mode_set) <= 1:
        mode_display = mode
    else:
        mode_display = "mixed(" + ", ".join(mode_set) + ")"
    print(f"decode mode: {mode_display}")
    if backend == "dicomsdl" and mode in ("decode_into", "to_array_view_copyto"):
        print("decode_into threads hint: -1 (all CPUs)")
    if backend == "dicomsdl" and mode == "to_array":
        j2k_forced = sorted(
            row.codec for row in stats if row.codec in _J2K_CODECS and row.mode == "decode_into"
        )
        if j2k_forced:
            print(
                "note: base table forces J2K codecs to decode_into(..., threads=-1): "
                + ", ".join(j2k_forced)
            )
    print(
        f"{'Codec':<5} {'Files':>5} {'Decodes':>8} {'Time(s)':>9} "
        f"{'ms/decode':>10} {'MPix/s':>10} {'MiB/s':>10} Label"
    )
    print("-" * 88)
    for row in stats:
        print(
            f"{row.codec:<5} {row.files:>5d} {row.decodes:>8d} {row.elapsed_s:>9.3f} "
            f"{row.ms_per_decode:>10.3f} {row.mpix_s:>10.3f} {row.mib_s:>10.3f} {row.label}"
        )

    total_rows = [row for row in stats if not _is_htj2k_codec(row.codec)]
    excluded_htj2k = len(stats) - len(total_rows)
    if not total_rows:
        total_rows = stats
        excluded_htj2k = 0

    total_decodes = sum(row.decodes for row in total_rows)
    total_pixels = sum(row.total_pixels for row in total_rows)
    total_bytes = sum(row.total_bytes for row in total_rows)
    total_elapsed = sum(row.elapsed_s for row in total_rows)
    total_mpix = (total_pixels / 1_000_000.0 / total_elapsed) if total_elapsed > 0.0 else 0.0
    total_mib = (total_bytes / (1024.0 * 1024.0) / total_elapsed) if total_elapsed > 0.0 else 0.0
    total_ms = (total_elapsed * 1000.0 / total_decodes) if total_decodes else 0.0

    print("-" * 88)
    print(
        f"{'TOTAL':<5} {sum(row.files for row in total_rows):>5d} {total_decodes:>8d} {total_elapsed:>9.3f} "
        f"{total_ms:>10.3f} {total_mpix:>10.3f} {total_mib:>10.3f}"
    )
    if excluded_htj2k > 0:
        print(f"note: TOTAL excludes {excluded_htj2k} HTJ2K codec row(s).")


def _build_comparison_rows(
    codecs: list[str],
    dicomsdl_stats: list[CodecStats],
    pydicom_stats: list[CodecStats],
    *,
    compression_ratio_by_codec: dict[str, float] | None = None,
    compression_pairs_by_codec: dict[str, int] | None = None,
) -> list[dict[str, float | int | str | None]]:
    dicomsdl_map = {row.codec: row for row in dicomsdl_stats}
    pydicom_map = {row.codec: row for row in pydicom_stats}

    rows: list[dict[str, float | int | str | None]] = []
    for codec in codecs:
        d = dicomsdl_map.get(codec)
        p = pydicom_map.get(codec)
        if d is None:
            continue

        compression_ratio = (
            float(compression_ratio_by_codec.get(codec, 0.0))
            if compression_ratio_by_codec is not None
            else 0.0
        )
        compression_pairs = (
            int(compression_pairs_by_codec.get(codec, 0))
            if compression_pairs_by_codec is not None
            else 0
        )

        row: dict[str, float | int | str | None] = {
            "codec": codec,
            "files": d.files,
            "decodes": d.decodes,
            "dicomsdl_ms_per_decode": d.ms_per_decode,
            "dicomsdl_mpix_s": d.mpix_s,
            "dicomsdl_elapsed_s": d.elapsed_s,
            "dicomsdl_total_pixels": d.total_pixels,
            "dicomsdl_total_bytes": d.total_bytes,
            "avg_compression_ratio_vs_ref": compression_ratio,
            "compression_pairs": compression_pairs,
        }

        if p is None:
            row["pydicom_ms_per_decode"] = None
            row["pydicom_mpix_s"] = None
            row["pydicom_elapsed_s"] = None
            row["pydicom_total_pixels"] = None
            row["pydicom_total_bytes"] = None
            row["speedup_dicomsdl_vs_pydicom"] = None
        else:
            speedup = (p.ms_per_decode / d.ms_per_decode) if d.ms_per_decode > 0.0 else 0.0
            row["pydicom_ms_per_decode"] = p.ms_per_decode
            row["pydicom_mpix_s"] = p.mpix_s
            row["pydicom_elapsed_s"] = p.elapsed_s
            row["pydicom_total_pixels"] = p.total_pixels
            row["pydicom_total_bytes"] = p.total_bytes
            row["speedup_dicomsdl_vs_pydicom"] = speedup

        rows.append(row)
    return rows


def _build_single_comparison_row(
    codec: str,
    dicomsdl_row: CodecStats,
    pydicom_row: CodecStats,
    *,
    compression_ratio: float = 0.0,
    compression_pairs: int = 0,
) -> dict[str, float | int | str]:
    speedup = (
        pydicom_row.ms_per_decode / dicomsdl_row.ms_per_decode
        if dicomsdl_row.ms_per_decode > 0.0
        else 0.0
    )
    return {
        "codec": codec,
        "files": dicomsdl_row.files,
        "decodes": dicomsdl_row.decodes,
        "dicomsdl_ms_per_decode": dicomsdl_row.ms_per_decode,
        "pydicom_ms_per_decode": pydicom_row.ms_per_decode,
        "dicomsdl_mpix_s": dicomsdl_row.mpix_s,
        "pydicom_mpix_s": pydicom_row.mpix_s,
        "dicomsdl_elapsed_s": dicomsdl_row.elapsed_s,
        "pydicom_elapsed_s": pydicom_row.elapsed_s,
        "dicomsdl_total_pixels": dicomsdl_row.total_pixels,
        "pydicom_total_pixels": pydicom_row.total_pixels,
        "dicomsdl_total_bytes": dicomsdl_row.total_bytes,
        "pydicom_total_bytes": pydicom_row.total_bytes,
        "speedup_dicomsdl_vs_pydicom": speedup,
        "avg_compression_ratio_vs_ref": float(compression_ratio),
        "compression_pairs": int(compression_pairs),
    }


def _format_optional_float(value: float | int | None, *, width: int, precision: int) -> str:
    if value is None:
        return f"{'n/a':>{width}}"
    return f"{float(value):>{width}.{precision}f}"


def _print_comparison_table(rows: list[dict[str, float | int | str | None]]) -> None:
    print("\n== Comparison (dicomsdl vs pydicom) ==")
    print(
        f"{'Codec':<5} {'Files':>5} {'Decodes':>8} {'dicomsdl ms':>12} {'pydicom ms':>11} "
        f"{'dicomsdl MPix/s':>16} {'pydicom MPix/s':>15} {'dcm/pyd x':>10} {'CR(ref/x)':>10}"
    )
    print("-" * 112)
    for row in rows:
        compression_pairs = int(row.get("compression_pairs", 0))
        if compression_pairs > 0:
            compression_ratio_text = f"{float(row.get('avg_compression_ratio_vs_ref', 0.0)):>10.2f}"
        else:
            compression_ratio_text = f"{'n/a':>10}"
        dicomsdl_ms_text = _format_optional_float(
            row.get("dicomsdl_ms_per_decode"), width=12, precision=3
        )
        pydicom_ms_text = _format_optional_float(
            row.get("pydicom_ms_per_decode"), width=11, precision=3
        )
        dicomsdl_mpix_text = _format_optional_float(row.get("dicomsdl_mpix_s"), width=16, precision=3)
        pydicom_mpix_text = _format_optional_float(row.get("pydicom_mpix_s"), width=15, precision=3)
        speedup_text = _format_optional_float(
            row.get("speedup_dicomsdl_vs_pydicom"), width=10, precision=2
        )
        print(
            f"{str(row['codec']):<5} {int(row['files']):>5d} {int(row['decodes']):>8d} "
            f"{dicomsdl_ms_text} {pydicom_ms_text} "
            f"{dicomsdl_mpix_text} {pydicom_mpix_text} "
            f"{speedup_text} {compression_ratio_text}"
        )

    base_rows = [row for row in rows if not str(row.get("codec", "")).endswith("*")]
    comparable_rows = [
        row
        for row in base_rows
        if row.get("pydicom_elapsed_s") is not None and not _is_htj2k_codec(str(row.get("codec", "")))
    ]
    excluded_htj2k = sum(1 for row in base_rows if _is_htj2k_codec(str(row.get("codec", ""))))
    excluded_missing = sum(
        1
        for row in base_rows
        if row.get("pydicom_elapsed_s") is None and not _is_htj2k_codec(str(row.get("codec", "")))
    )

    if comparable_rows:
        total_decodes = sum(int(row["decodes"]) for row in comparable_rows)
        total_dicomsdl_elapsed = sum(float(row["dicomsdl_elapsed_s"]) for row in comparable_rows)
        total_pydicom_elapsed = sum(float(row["pydicom_elapsed_s"]) for row in comparable_rows)
        total_dicomsdl_pixels = sum(int(row["dicomsdl_total_pixels"]) for row in comparable_rows)
        total_pydicom_pixels = sum(int(row["pydicom_total_pixels"]) for row in comparable_rows)

        total_dicomsdl_ms = (
            total_dicomsdl_elapsed * 1000.0 / total_decodes if total_decodes > 0 else 0.0
        )
        total_pydicom_ms = (
            total_pydicom_elapsed * 1000.0 / total_decodes if total_decodes > 0 else 0.0
        )
        total_dicomsdl_mpix = (
            total_dicomsdl_pixels / 1_000_000.0 / total_dicomsdl_elapsed
            if total_dicomsdl_elapsed > 0.0
            else 0.0
        )
        total_pydicom_mpix = (
            total_pydicom_pixels / 1_000_000.0 / total_pydicom_elapsed
            if total_pydicom_elapsed > 0.0
            else 0.0
        )
        total_speedup = (total_pydicom_ms / total_dicomsdl_ms) if total_dicomsdl_ms > 0.0 else 0.0
        total_compression_pairs = sum(int(row.get("compression_pairs", 0)) for row in comparable_rows)
        if total_compression_pairs > 0:
            total_compression_ratio = sum(
                float(row.get("avg_compression_ratio_vs_ref", 0.0)) *
                int(row.get("compression_pairs", 0))
                for row in comparable_rows
            ) / total_compression_pairs
            total_compression_text = f"{total_compression_ratio:>10.2f}"
        else:
            total_compression_text = f"{'n/a':>10}"

        print("-" * 112)
        print(
            f"{'TOTAL':<5} {sum(int(row['files']) for row in comparable_rows):>5d} {total_decodes:>8d} "
            f"{total_dicomsdl_ms:>12.3f} {total_pydicom_ms:>11.3f} "
            f"{total_dicomsdl_mpix:>16.3f} {total_pydicom_mpix:>15.3f} "
            f"{total_speedup:>10.2f} {total_compression_text}"
        )
    if base_rows and len(comparable_rows) != len(base_rows):
        notes: list[str] = []
        if excluded_htj2k > 0:
            notes.append(f"{excluded_htj2k} HTJ2K codec(s)")
        if excluded_missing > 0:
            notes.append(f"{excluded_missing} codec(s) without pydicom decode results")
        if notes:
            print("note: TOTAL excludes " + "; ".join(notes) + ".")

    if any(str(row.get("codec", "")).endswith("*") for row in rows):
        print(
            f"* {_RAW_BEST_CODEC}* uses RAW best-path modes only "
            f"(dicomsdl={_RAW_BEST_DICOMSDL_MODE}, pydicom={_RAW_BEST_PYDICOM_MODE})."
        )


def _print_j2k_multicpu_report(
    stats: list[CodecStats], comparison_rows: list[dict[str, float | int | str]], threads: int
) -> None:
    if not stats:
        return

    print("\n== dicomsdl J2K Multi-CPU ==")
    print(f"mode: decode_into, threads={threads}")
    print(
        f"{'Codec':<8} {'Files':>5} {'Decodes':>8} {'Time(s)':>9} "
        f"{'ms/decode':>10} {'MPix/s':>10} {'MiB/s':>10} Label"
    )
    print("-" * 92)
    for row in stats:
        print(
            f"{row.codec:<8} {row.files:>5d} {row.decodes:>8d} {row.elapsed_s:>9.3f} "
            f"{row.ms_per_decode:>10.3f} {row.mpix_s:>10.3f} {row.mib_s:>10.3f} {row.label}"
        )

    if not comparison_rows:
        return

    print("\n== J2K Multi-CPU vs pydicom ==")
    print(
        f"{'Codec':<8} {'dicomsdl ms':>12} {'pydicom ms':>11} "
        f"{'dicomsdl MPix/s':>16} {'pydicom MPix/s':>15} {'dcm/pyd x':>10}"
    )
    print("-" * 84)
    for row in comparison_rows:
        print(
            f"{str(row['codec']):<8} {float(row['dicomsdl_ms_per_decode']):>12.3f} "
            f"{float(row['pydicom_ms_per_decode']):>11.3f} "
            f"{float(row['dicomsdl_mpix_s']):>16.3f} {float(row['pydicom_mpix_s']):>15.3f} "
            f"{float(row['speedup_dicomsdl_vs_pydicom']):>10.2f}"
        )


def _parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Benchmark WG04 pixel decode by codec. "
            "Supports dicomsdl, pydicom, or both backends."
        )
    )
    parser.add_argument(
        "root",
        nargs="?",
        type=Path,
        default=None,
        help=f"WG04 IMAGES root (default: ${_WG04_IMAGES_ENV} or {_DEFAULT_WG04_IMAGES_ROOT})",
    )
    parser.add_argument(
        "--backend",
        choices=("dicomsdl", "pydicom", "both"),
        default="dicomsdl",
        help="Backend to benchmark.",
    )
    parser.add_argument(
        "--codec",
        dest="codecs",
        action="append",
        choices=_CODEC_ORDER,
        help="Codec directory to benchmark (repeatable). Defaults to all codecs.",
    )
    parser.add_argument("--warmup", type=int, default=1, help="Warm-up decode passes per codec.")
    parser.add_argument(
        "--repeat",
        "-r",
        type=int,
        default=5,
        help="Measured decode passes per codec.",
    )
    parser.add_argument(
        "--scaled",
        action="store_true",
        help="Use scaled=True (dicomsdl only).",
    )
    parser.add_argument(
        "--reuse-output",
        action="store_true",
        help=(
            "Backward-compatible alias for --dicomsdl-mode=decode_into. "
            "Uses decode_into(..., threads=-1) by default. "
            "Ignored for non-dicomsdl backends."
        ),
    )
    parser.add_argument(
        "--reuse-output-pydicom",
        action="store_true",
        help=(
            "Backward-compatible alias for --pydicom-mode=reuse_output. "
            "Reuse preallocated output arrays for pydicom. "
            "Uncompressed files use numpy_handler(read_only)+np.copyto; "
            "compressed files fall back to pixel_array+np.copyto."
        ),
    )
    parser.add_argument(
        "--dicomsdl-mode",
        choices=_DICOMSDL_MODE_ORDER,
        default="to_array",
        help="dicomsdl decode mode.",
    )
    parser.add_argument(
        "--dicomsdl-htj2k-decoder",
        choices=("auto", "openjph", "openjpeg"),
        default="auto",
        help="HTJ2K backend selection for dicomsdl decode paths.",
    )
    parser.add_argument(
        "--pydicom-mode",
        choices=_PYDICOM_MODE_ORDER,
        default="pixel_array",
        help="pydicom decode mode.",
    )
    parser.add_argument(
        "--include-j2k-multicpu",
        action="store_true",
        help=(
            "Add extra dicomsdl J2K rows using decode_into(..., threads=-1), "
            "reported separately from the base table."
        ),
    )
    parser.add_argument(
        "--j2k-multicpu-threads",
        type=int,
        default=-1,
        help="Thread count hint for --include-j2k-multicpu (-1: all CPUs).",
    )
    parser.add_argument("--json", dest="json_path", type=Path, help="Write benchmark report to JSON.")
    parser.add_argument("--verbose", "-v", action="store_true", help="Print per-file decode details.")
    parser.add_argument(
        "--list-codecs",
        action="store_true",
        help="Print available codec directory names and exit.",
    )
    parser.add_argument(
        "--list-backends",
        action="store_true",
        help="Print available backends and exit.",
    )
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = _parse_args(argv)

    if args.list_codecs:
        for codec in _CODEC_ORDER:
            print(f"{codec:5s} {_CODEC_LABEL[codec]}")
        return 0

    if args.list_backends:
        for backend in _BACKEND_ORDER:
            print(backend)
        return 0

    if args.warmup < 0:
        print("--warmup must be >= 0", file=sys.stderr)
        return 2
    if args.repeat <= 0:
        print("--repeat must be > 0", file=sys.stderr)
        return 2
    if args.include_j2k_multicpu and args.j2k_multicpu_threads < -1:
        print("--j2k-multicpu-threads must be >= -1", file=sys.stderr)
        return 2

    selected_backends = list(_BACKEND_ORDER) if args.backend == "both" else [args.backend]
    dicomsdl_mode = "decode_into" if args.reuse_output else args.dicomsdl_mode
    dicomsdl_htj2k_decoder = args.dicomsdl_htj2k_decoder
    pydicom_mode = "reuse_output" if args.reuse_output_pydicom else args.pydicom_mode

    if args.scaled and "pydicom" in selected_backends:
        print("--scaled is only supported when backend=dicomsdl", file=sys.stderr)
        return 2
    if args.scaled and dicomsdl_mode in ("to_array_view", "to_array_view_copyto"):
        print(
            "--scaled cannot be combined with --dicomsdl-mode=to_array_view or to_array_view_copyto",
            file=sys.stderr,
        )
        return 2
    if args.reuse_output and "dicomsdl" not in selected_backends:
        print("--reuse-output requires backend=dicomsdl or backend=both", file=sys.stderr)
        return 2
    if args.reuse_output_pydicom and "pydicom" not in selected_backends:
        print("--reuse-output-pydicom requires backend=pydicom or backend=both", file=sys.stderr)
        return 2

    root = _resolve_root(args.root)
    if not root.exists():
        print(f"WG04 root does not exist: {root}", file=sys.stderr)
        return 1

    codecs = args.codecs if args.codecs else list(_CODEC_ORDER)
    stats_by_backend: dict[str, list[CodecStats]] = {}
    skipped_codecs_by_backend: dict[str, dict[str, str]] = {}
    modules_by_backend: dict[str, Any] = {}

    for backend in selected_backends:
        try:
            module = _import_backend(backend)
        except Exception as exc:
            print(f"failed to import {backend}: {exc}", file=sys.stderr)
            return 1
        modules_by_backend[backend] = module

        backend_stats: list[CodecStats] = []
        skipped_for_backend: dict[str, str] = {}
        for codec in codecs:
            try:
                codec_dicomsdl_mode = dicomsdl_mode
                if backend == "dicomsdl" and codec in _J2K_CODECS and dicomsdl_mode == "to_array":
                    codec_dicomsdl_mode = "decode_into"

                paths = _codec_paths(root, codec)
                inputs = _preload_inputs(paths, backend, module)
                row = _benchmark_codec(
                    backend,
                    codec,
                    inputs,
                    warmup=args.warmup,
                    repeat=args.repeat,
                    scaled=args.scaled,
                    dicomsdl_mode=codec_dicomsdl_mode,
                    dicomsdl_htj2k_decoder=dicomsdl_htj2k_decoder,
                    pydicom_mode=pydicom_mode,
                    verbose=args.verbose,
                    decoder_threads=-1,
                )
                backend_stats.append(row)
            except Exception as exc:
                if backend == "pydicom" and codec in _HTJ2K_CODECS:
                    reason = str(exc).strip() or exc.__class__.__name__
                    skipped_for_backend[codec] = reason
                    print(
                        f"note: skipping pydicom codec {codec} (HTJ2K unsupported): {reason}",
                        file=sys.stderr,
                    )
                    continue
                print(f"failed to benchmark {backend} codec {codec}: {exc}", file=sys.stderr)
                return 1

        stats_by_backend[backend] = backend_stats
        if skipped_for_backend:
            skipped_codecs_by_backend[backend] = skipped_for_backend
        _print_backend_report(
            root,
            backend,
            backend_stats,
            scaled=args.scaled,
            mode=(dicomsdl_mode if backend == "dicomsdl" else pydicom_mode),
        )

    comparison_rows: list[dict[str, float | int | str | None]] = []
    raw_best_stats_by_backend: dict[str, CodecStats] = {}
    compression_ratio_by_codec: dict[str, float] = {}
    compression_pairs_by_codec: dict[str, int] = {}
    if "dicomsdl" in selected_backends and "pydicom" in selected_backends:
        compression_ratio_by_codec, compression_pairs_by_codec = _build_compression_ratio_maps(
            root, codecs
        )
    if "dicomsdl" in stats_by_backend and "pydicom" in stats_by_backend:
        comparison_rows = _build_comparison_rows(
            codecs,
            stats_by_backend["dicomsdl"],
            stats_by_backend["pydicom"],
            compression_ratio_by_codec=compression_ratio_by_codec,
            compression_pairs_by_codec=compression_pairs_by_codec,
        )

        if _RAW_BEST_CODEC in codecs:
            raw_paths = _codec_paths(root, _RAW_BEST_CODEC)

            dicomsdl_inputs = _preload_inputs(raw_paths, "dicomsdl", modules_by_backend["dicomsdl"])
            raw_best_stats_by_backend["dicomsdl"] = _benchmark_codec(
                "dicomsdl",
                _RAW_BEST_CODEC,
                dicomsdl_inputs,
                warmup=args.warmup,
                repeat=args.repeat,
                scaled=args.scaled,
                dicomsdl_mode=_RAW_BEST_DICOMSDL_MODE,
                dicomsdl_htj2k_decoder=dicomsdl_htj2k_decoder,
                pydicom_mode=pydicom_mode,
                verbose=args.verbose,
                decoder_threads=-1,
            )

            pydicom_inputs = _preload_inputs(raw_paths, "pydicom", modules_by_backend["pydicom"])
            raw_best_stats_by_backend["pydicom"] = _benchmark_codec(
                "pydicom",
                _RAW_BEST_CODEC,
                pydicom_inputs,
                warmup=args.warmup,
                repeat=args.repeat,
                scaled=args.scaled,
                dicomsdl_mode=dicomsdl_mode,
                dicomsdl_htj2k_decoder=dicomsdl_htj2k_decoder,
                pydicom_mode=_RAW_BEST_PYDICOM_MODE,
                verbose=args.verbose,
                decoder_threads=-1,
            )

            comparison_rows.append(
                _build_single_comparison_row(
                    f"{_RAW_BEST_CODEC}*",
                    raw_best_stats_by_backend["dicomsdl"],
                    raw_best_stats_by_backend["pydicom"],
                    compression_ratio=float(compression_ratio_by_codec.get(_RAW_BEST_CODEC, 0.0)),
                    compression_pairs=int(compression_pairs_by_codec.get(_RAW_BEST_CODEC, 0)),
                )
            )

        _print_comparison_table(comparison_rows)

    j2k_multicpu_stats: list[CodecStats] = []
    j2k_multicpu_comparison: list[dict[str, float | int | str]] = []
    if args.include_j2k_multicpu and "dicomsdl" in selected_backends:
        for codec in _J2K_CODECS:
            if codec not in codecs:
                continue
            paths = _codec_paths(root, codec)
            dicomsdl_inputs = _preload_inputs(paths, "dicomsdl", modules_by_backend["dicomsdl"])
            mt_row = _benchmark_codec(
                "dicomsdl",
                codec,
                dicomsdl_inputs,
                warmup=args.warmup,
                repeat=args.repeat,
                scaled=args.scaled,
                dicomsdl_mode="decode_into",
                dicomsdl_htj2k_decoder=dicomsdl_htj2k_decoder,
                pydicom_mode=pydicom_mode,
                verbose=args.verbose,
                decoder_threads=args.j2k_multicpu_threads,
            )
            mt_row.codec = f"{codec}_MT"
            mt_row.label = (
                f"{_CODEC_LABEL.get(codec, codec)} "
                f"(decode_into, threads={args.j2k_multicpu_threads})"
            )
            j2k_multicpu_stats.append(mt_row)

            if "pydicom" in stats_by_backend:
                pydicom_map = {row.codec: row for row in stats_by_backend["pydicom"]}
                if codec in pydicom_map:
                    j2k_multicpu_comparison.append(
                        _build_single_comparison_row(mt_row.codec, mt_row, pydicom_map[codec])
                    )

        _print_j2k_multicpu_report(
            j2k_multicpu_stats, j2k_multicpu_comparison, args.j2k_multicpu_threads
        )

    if args.json_path:
        payload = {
            "wg04_root": str(root),
            "scaled": bool(args.scaled),
            "warmup": int(args.warmup),
            "repeat": int(args.repeat),
            "backend": args.backend,
            "reuse_output": bool(args.reuse_output),
            "reuse_output_pydicom": bool(args.reuse_output_pydicom),
            "dicomsdl_mode": dicomsdl_mode,
            "dicomsdl_htj2k_decoder": dicomsdl_htj2k_decoder,
            "pydicom_mode": pydicom_mode,
            "dicomsdl_decode_into_threads_default": -1,
            "codecs": codecs,
            "results": {
                backend: [asdict(row) for row in rows]
                for backend, rows in stats_by_backend.items()
            },
            "raw_best_ref": {
                backend: asdict(row) for backend, row in raw_best_stats_by_backend.items()
            },
            "comparison": comparison_rows,
            "skipped_codecs_by_backend": skipped_codecs_by_backend,
            "j2k_multicpu": {
                "enabled": bool(args.include_j2k_multicpu),
                "threads": int(args.j2k_multicpu_threads),
                "dicomsdl": [asdict(row) for row in j2k_multicpu_stats],
                "comparison_vs_pydicom": j2k_multicpu_comparison,
            },
        }
        args.json_path.parent.mkdir(parents=True, exist_ok=True)
        args.json_path.write_text(json.dumps(payload, indent=2), encoding="utf-8")
        print(f"wrote JSON report: {args.json_path}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
