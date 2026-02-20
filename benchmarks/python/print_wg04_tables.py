#!/usr/bin/env python3
"""Print markdown tables from WG04 benchmark JSON reports."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


_MAIN_CODEC_ORDER: tuple[str, ...] = (
    "REF",
    "RLE",
    "J2KR",
    "J2KI",
    "JLSL",
    "JLSN",
    "JPLL",
    "JPLY",
)
_HTJ2K_CODECS: tuple[str, ...] = ("htj2kll", "htj2kly")


def _load_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def _codec_row_map(payload: dict[str, Any], backend: str) -> dict[str, dict[str, Any]]:
    rows = payload.get("results", {}).get(backend, [])
    return {str(row.get("codec", "")): row for row in rows}


def _compression_maps(payload: dict[str, Any]) -> tuple[dict[str, float], dict[str, int]]:
    cr_by_codec: dict[str, float] = {}
    pairs_by_codec: dict[str, int] = {}
    for row in payload.get("comparison", []):
        codec = str(row.get("codec", ""))
        if not codec or codec.endswith("*"):
            continue
        cr_by_codec[codec] = float(row.get("avg_compression_ratio_vs_ref", 0.0))
        pairs_by_codec[codec] = int(row.get("compression_pairs", 0))
    return cr_by_codec, pairs_by_codec


def _print_main_table(main_payload: dict[str, Any]) -> None:
    dcm = _codec_row_map(main_payload, "dicomsdl")
    pyd = _codec_row_map(main_payload, "pydicom")
    cr_by_codec, pairs_by_codec = _compression_maps(main_payload)

    print("| Codec | dicomsdl ms/decode | pydicom ms/decode | dcm/pyd x | CR(ref/x) |")
    print("| --- | ---: | ---: | ---: | ---: |")

    total_decodes = 0
    total_dcm_elapsed = 0.0
    total_pyd_elapsed = 0.0
    total_cr_weight = 0.0
    total_cr_pairs = 0

    for codec in _MAIN_CODEC_ORDER:
        d = dcm.get(codec)
        p = pyd.get(codec)
        if d is None or p is None:
            continue
        d_ms = float(d.get("ms_per_decode", 0.0))
        p_ms = float(p.get("ms_per_decode", 0.0))
        speedup = (p_ms / d_ms) if d_ms > 0.0 else 0.0
        cr = float(cr_by_codec.get(codec, 0.0))

        print(f"| {codec} | {d_ms:.3f} | {p_ms:.3f} | {speedup:.2f} | {cr:.2f} |")

        decodes = int(d.get("decodes", 0))
        total_decodes += decodes
        total_dcm_elapsed += float(d.get("elapsed_s", 0.0))
        total_pyd_elapsed += float(p.get("elapsed_s", 0.0))

        pairs = int(pairs_by_codec.get(codec, 0))
        if pairs > 0:
            total_cr_weight += cr * pairs
            total_cr_pairs += pairs

    if total_decodes > 0:
        total_dcm_ms = (total_dcm_elapsed * 1000.0) / total_decodes
        total_pyd_ms = (total_pyd_elapsed * 1000.0) / total_decodes
        total_speedup = (total_pyd_ms / total_dcm_ms) if total_dcm_ms > 0.0 else 0.0
        total_cr = (total_cr_weight / total_cr_pairs) if total_cr_pairs > 0 else 0.0
        print(f"| TOTAL | {total_dcm_ms:.3f} | {total_pyd_ms:.3f} | {total_speedup:.2f} | {total_cr:.2f} |")


def _print_htj2k_openjpeg_vs_pydicom_table(
    main_payload: dict[str, Any], openjpeg_payload: dict[str, Any]
) -> None:
    dcm_openjpeg = _codec_row_map(openjpeg_payload, "dicomsdl")
    pyd = _codec_row_map(main_payload, "pydicom")
    cr_by_codec, pairs_by_codec = _compression_maps(main_payload)

    print("\n| Variant | dicomsdl ms/decode | pydicom ms/decode | dcm/pyd x | CR(ref/x) |")
    print("| --- | ---: | ---: | ---: | ---: |")

    row_count = 0
    for codec in _HTJ2K_CODECS:
        d = dcm_openjpeg.get(codec)
        p = pyd.get(codec)
        if d is None and p is None:
            continue

        d_ms = float(d.get("ms_per_decode", 0.0)) if d is not None else None
        p_ms = float(p.get("ms_per_decode", 0.0)) if p is not None else None
        speedup = (p_ms / d_ms) if d_ms is not None and p_ms is not None and d_ms > 0.0 else None
        pairs = int(pairs_by_codec.get(codec, 0))
        cr = float(cr_by_codec.get(codec, 0.0)) if pairs > 0 else None

        d_ms_text = f"{d_ms:.3f}" if d_ms is not None else "n/a"
        p_ms_text = f"{p_ms:.3f}" if p_ms is not None else "n/a"
        speedup_text = f"{speedup:.2f}" if speedup is not None else "n/a"
        cr_text = f"{cr:.2f}" if cr is not None else "n/a"

        print(f"| {codec} (openjpeg) | {d_ms_text} | {p_ms_text} | {speedup_text} | {cr_text} |")
        row_count += 1

    if row_count == 0:
        print("| htj2kll (openjpeg) | n/a | n/a | n/a | n/a |")
        print("| htj2kly (openjpeg) | n/a | n/a | n/a | n/a |")


def _print_htj2k_table(
    main_payload: dict[str, Any],
    openjpeg_payload: dict[str, Any],
    openjph_payload: dict[str, Any],
) -> None:
    cr_by_codec, _ = _compression_maps(main_payload)
    openjpeg_rows = _codec_row_map(openjpeg_payload, "dicomsdl")
    openjph_rows = _codec_row_map(openjph_payload, "dicomsdl")

    print("\n| Variant | dicomsdl ms/decode | CR(ref/x) |")
    print("| --- | ---: | ---: |")

    for codec in _HTJ2K_CODECS:
        cr = float(cr_by_codec.get(codec, 0.0))
        oj_row = openjpeg_rows.get(codec)
        if oj_row is not None:
            print(
                f"| {codec} (openjpeg) | {float(oj_row.get('ms_per_decode', 0.0)):.3f} | {cr:.2f} |"
            )
        ojph_row = openjph_rows.get(codec)
        if ojph_row is not None:
            print(
                f"| {codec} (openjph) | {float(ojph_row.get('ms_per_decode', 0.0)):.3f} | {cr:.2f} |"
            )


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Print WG04 benchmark markdown tables.")
    parser.add_argument("--main-json", type=Path, required=True, help="Full WG04 comparison JSON path.")
    parser.add_argument(
        "--htj2k-openjpeg-json",
        type=Path,
        required=True,
        help="HTJ2K openjpeg-only JSON path.",
    )
    parser.add_argument(
        "--htj2k-openjph-json",
        type=Path,
        required=True,
        help="HTJ2K openjph-only JSON path.",
    )
    return parser.parse_args()


def main() -> int:
    args = _parse_args()
    main_payload = _load_json(args.main_json)
    openjpeg_payload = _load_json(args.htj2k_openjpeg_json)
    openjph_payload = _load_json(args.htj2k_openjph_json)

    _print_main_table(main_payload)
    _print_htj2k_openjpeg_vs_pydicom_table(main_payload, openjpeg_payload)
    _print_htj2k_table(main_payload, openjpeg_payload, openjph_payload)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
