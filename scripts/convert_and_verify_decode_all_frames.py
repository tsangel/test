from __future__ import annotations

import argparse
import json
import os
import pathlib
import sys
from dataclasses import asdict, dataclass


ROOT = pathlib.Path(__file__).resolve().parents[1]
BINDINGS_DIR = ROOT / "bindings" / "python"
BUILD_GLOBS = ("build*",)
NATIVE_PATTERNS = ("_dicomsdl*.pyd", "_dicomsdl*.so", "_dicomsdl*.dylib")


def _find_native_module() -> pathlib.Path:
    candidates: list[pathlib.Path] = []
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


def _find_build_root(native_module: pathlib.Path) -> pathlib.Path | None:
    for parent in native_module.parents:
        if parent == ROOT:
            return None
        if parent.name.startswith("build"):
            return parent
    return None


def _read_cmake_compiler_bin(build_root: pathlib.Path | None) -> pathlib.Path | None:
    if build_root is None:
        return None
    cache_path = build_root / "CMakeCache.txt"
    if not cache_path.exists():
        return None
    for line in cache_path.read_text(encoding="utf-8", errors="replace").splitlines():
        for prefix in ("CMAKE_CXX_COMPILER:FILEPATH=", "CMAKE_CXX_COMPILER:STRING="):
            if not line.startswith(prefix):
                continue
            compiler_path = pathlib.Path(line[len(prefix) :].strip())
            if compiler_path.exists():
                return compiler_path.parent
    return None


def _configure_import_path(native_module: pathlib.Path) -> None:
    desired_entries = [str(BINDINGS_DIR), str(native_module.parent)]
    for entry in reversed(desired_entries):
        if entry not in sys.path:
            sys.path.insert(0, entry)


def _configure_windows_dll_dirs(native_module: pathlib.Path) -> None:
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


DEFAULT_INPUTS = [
    ROOT.parent / "sample" / "ENH_MR.dcm",
    ROOT.parent / "sample" / "multiframe" / "multiframe.dcm",
]

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
    "JPEGXLLossless": {"type": "jpegxl", "distance": 0.0},
    "HTJ2KLossless": {"type": "htj2k"},
    "HTJ2KLosslessRPCL": {"type": "htj2k"},
    "RLELossless": {"type": "rle"},
}


@dataclass
class ConversionResult:
    input_path: str
    transfer_syntax: str
    output_path: str | None
    status: str
    detail: str
    frames: int
    rows: int
    cols: int
    samples_per_pixel: int
    bits_stored: int


def _resolve_inputs(args_inputs: list[str] | None) -> list[pathlib.Path]:
    if not args_inputs:
        return DEFAULT_INPUTS
    return [(ROOT / path).resolve() for path in args_inputs]


def _build_output_dir(output_dir: str | None) -> pathlib.Path:
    if output_dir:
        return (ROOT / output_dir).resolve()
    build_root = _find_build_root(native_module_path)
    if build_root is not None:
        return build_root / "decode_all_frames_check"
    return ROOT / "decode_all_frames_check"


def _filtered_transfer_syntax_uids() -> list[dicom.Uid]:
    out: list[dicom.Uid] = []
    for uid in dicom.transfer_syntax_uids_encode_supported():
        keyword = uid.keyword or uid.value
        if keyword in UNCOMPRESSED_KEYWORDS:
            continue
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


def _frame_reference(df: dicom.DicomFile, plan: dicom.DecodePlan) -> np.ndarray:
    frames = plan.frames
    if frames <= 0:
        raise RuntimeError("decode plan does not describe any frames")
    if frames == 1:
        return np.expand_dims(df.to_array(frame=0, plan=plan), axis=0)
    return np.stack([df.to_array(frame=i, plan=plan) for i in range(frames)], axis=0)


def _verify_decode_all_frames(df: dicom.DicomFile) -> None:
    options = dicom.DecodeOptions(worker_threads=2)
    plan = df.create_decode_plan(options)
    expected = _frame_reference(df, plan)
    out = np.empty(plan.shape(frame=-1), dtype=plan.dtype)
    returned = df.decode_into(out, frame=-1, plan=plan)
    if returned is not out:
        raise RuntimeError("decode_into(frame=-1) did not return the provided output array")
    if out.shape != expected.shape:
        raise RuntimeError(
            f"decode_all_frames shape mismatch: got {out.shape}, expected {expected.shape}"
        )
    if not np.array_equal(out, expected):
        raise RuntimeError("decode_all_frames output does not match per-frame decode")


def _is_unavailable_encoder_error(detail: str) -> bool:
    return "status=unsupported" in detail and "stage=plugin_lookup" in detail


def _convert_one(
    input_path: pathlib.Path, uid: dicom.Uid, output_dir: pathlib.Path
) -> ConversionResult:
    df = dicom.read_file(str(input_path))
    baseline_plan = df.create_decode_plan()
    keyword = uid.keyword or uid.value
    skip_reason = _skip_reason(
        keyword,
        baseline_plan.bits_stored,
        baseline_plan.samples_per_pixel,
    )
    if skip_reason is not None:
        return ConversionResult(
            input_path=str(input_path),
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

    output_path = output_dir / f"{input_path.stem}_{keyword}.dcm"
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
        return ConversionResult(
            input_path=str(input_path),
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
        return ConversionResult(
            input_path=str(input_path),
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
        return ConversionResult(
            input_path=str(input_path),
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


def run(inputs: list[pathlib.Path], output_dir: pathlib.Path) -> list[ConversionResult]:
    output_dir.mkdir(parents=True, exist_ok=True)
    results: list[ConversionResult] = []
    transfer_syntaxes = _filtered_transfer_syntax_uids()

    for input_path in inputs:
        if not input_path.exists():
            results.append(
                ConversionResult(
                    input_path=str(input_path),
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
            continue

        print(f"[input] {input_path}")
        for uid in transfer_syntaxes:
            keyword = uid.keyword or uid.value
            result = _convert_one(input_path, uid, output_dir)
            results.append(result)
            print(f"  [{result.status:13}] {keyword}: {result.detail}")

    summary_path = output_dir / "summary.json"
    summary_path.write_text(
        json.dumps([asdict(result) for result in results], indent=2),
        encoding="utf-8",
    )
    print(f"[summary] {summary_path}")
    return results


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Convert sample multi-frame DICOM files to each supported compressed "
            "transfer syntax and verify decode_all_frames via decode_into(frame=-1)."
        )
    )
    parser.add_argument(
        "inputs",
        nargs="*",
        help=(
            "Input paths relative to the repository root. "
            "Defaults to ../sample/ENH_MR.dcm and ../sample/multiframe/multiframe.dcm."
        ),
    )
    parser.add_argument(
        "--output-dir",
        help=(
            "Output directory relative to the repository root. "
            "Defaults to <latest build>/decode_all_frames_check."
        ),
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    inputs = _resolve_inputs(args.inputs)
    output_dir = _build_output_dir(args.output_dir)
    results = run(inputs, output_dir)

    failed = [
        result
        for result in results
        if result.status in {"missing_input", "convert_failed", "verify_failed"}
    ]
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
