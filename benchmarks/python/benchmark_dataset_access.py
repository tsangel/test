"""
Micro-benchmarks for DataSet access patterns across dicomsdl and pydicom.

This mirrors the measurements documented in docs/python_dataset_access_benchmarks.md.
Run:
    python benchmarks/python/benchmark_dataset_access.py
"""

from __future__ import annotations

import sys
import time
from pathlib import Path
from typing import Callable, List, Tuple


def bench(label: str, func: Callable[[], object], iterations: int) -> Tuple[str, float]:
    # Warm-up
    func()
    t0 = time.perf_counter()
    for _ in range(iterations):
        func()
    dt = time.perf_counter() - t0
    return label, (dt * 1e6) / iterations  # microseconds per call


def run_dicomsdl(sample_path: Path, which: str = "both") -> List[Tuple[str, float]]:
    import dicomsdl as dicom

    ds = dicom.read_file(str(sample_path))
    rows_exprs = [
        ("dicomsdl ds.Rows", lambda: ds.Rows),
        ("dicomsdl ds[0x00280010]", lambda: ds[0x00280010]),
        ("dicomsdl ds['Rows']", lambda: ds["Rows"]),
        ("dicomsdl getde to_long", lambda: ds.get_dataelement(0x00280010).to_long()),
        ("dicomsdl getde get_value", lambda: ds.get_dataelement(0x00280010).get_value()),
    ]

    seq_exprs = [
        ("dicomsdl packed path", lambda: ds["00540016.0.00181075"]),
        (
            "dicomsdl string path",
            lambda: ds["RadiopharmaceuticalInformationSequence.0.RadionuclideHalfLife"],
        ),
        (
            "dicomsdl attr chain",
            lambda: ds.RadiopharmaceuticalInformationSequence[0].RadionuclideHalfLife,
        ),
        (
            "dicomsdl get_value chain",
            lambda: ds.get_dataelement(0x00540016)
            .sequence[0]
            .get_dataelement(0x00181075)
            .get_value(),
        ),
        (
            "dicomsdl to_long chain",
            lambda: ds.get_dataelement(0x00540016)
            .sequence[0]
            .get_dataelement(0x00181075)
            .to_long(),
        ),
    ]

    results: List[Tuple[str, float]] = []
    if which in ("both", "scalars"):
        for label, fn in rows_exprs:
            results.append(bench(label, fn, iterations=100_000))
    if which in ("both", "sequence"):
        for label, fn in seq_exprs:
            results.append(bench(label, fn, iterations=5_000))
    return results


def run_pydicom(sample_path: Path, which: str = "both") -> List[Tuple[str, float]]:
    import pydicom

    ds = pydicom.dcmread(str(sample_path), stop_before_pixels=True)

    def to_int(v):
        if isinstance(v, bytes):
            return int.from_bytes(v, byteorder="little", signed=False)
        return int(v)

    scalar_exprs = [
        ("pydicom get_item().value", lambda: ds.get_item((0x0028, 0x0010)).value),
        ("pydicom int(get_item().value)", lambda: to_int(ds.get_item((0x0028, 0x0010)).value)),
        ("pydicom ds[(tag)].value", lambda: ds[(0x0028, 0x0010)].value),
        ("pydicom int(ds[(tag)].value)", lambda: to_int(ds[(0x0028, 0x0010)].value)),
        ("pydicom ds.Rows", lambda: ds.Rows),
    ]
    results: List[Tuple[str, float]] = []
    if which in ("both", "scalars"):
        for label, fn in scalar_exprs:
            results.append(bench(label, fn, iterations=100_000))
    return results


def main() -> int:
    if len(sys.argv) > 1:
        sample = Path(sys.argv[1]).expanduser().resolve()
        which = "both"
        if len(sys.argv) > 2:
            which = sys.argv[2]
    else:
        sample = Path(__file__).resolve().parents[2] / "sample" / (
            "1.2.840.113619.2.99.1234.1210123180.675655_0000_000034_121066021209fb.dcm"
        )
        which = "both"
    if not sample.exists():
        print(f"Sample file not found: {sample}", file=sys.stderr)
        return 1

    print(f"Using sample: {sample}")

    try:
        dicomsdl_results = run_dicomsdl(sample, which=which)
        if dicomsdl_results:
            print("\n== dicomsdl ==")
            scalar_labels = {
                "dicomsdl ds.Rows",
                "dicomsdl ds[0x00280010]",
                "dicomsdl ds['Rows']",
                "dicomsdl getde to_long",
                "dicomsdl getde get_value",
            }
            sequence_labels = {
                "dicomsdl packed path",
                "dicomsdl string path",
                "dicomsdl attr chain",
                "dicomsdl get_value chain",
                "dicomsdl to_long chain",
            }
            scalar_res = [(l, u) for l, u in dicomsdl_results if l in scalar_labels]
            seq_res = [(l, u) for l, u in dicomsdl_results if l in sequence_labels]
            if scalar_res:
                print("  -- Scalars --")
                for label, us in sorted(scalar_res, key=lambda x: x[1]):
                    print(f"{label:40s} {us:6.2f} µs/call")
            if seq_res:
                print("  -- Sequence --")
                for label, us in sorted(seq_res, key=lambda x: x[1]):
                    print(f"{label:40s} {us:6.2f} µs/call")
    except Exception as e:
        print(f"dicomsdl benchmark skipped: {e}", file=sys.stderr)

    try:
        pydicom_results = run_pydicom(sample, which=which)
        if pydicom_results:
            print("\n== pydicom ==")
            print("  -- Scalars --")
            for label, us in sorted(pydicom_results, key=lambda x: x[1]):
                print(f"{label:40s} {us:6.2f} µs/call")
    except Exception as e:
        print(f"pydicom benchmark skipped: {e}", file=sys.stderr)

    return 0


if __name__ == "__main__":
    sys.exit(main())
