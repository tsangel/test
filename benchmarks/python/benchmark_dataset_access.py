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
from typing import Callable, Iterable, List, Tuple


def bench(label: str, func: Callable[[], object], iterations: int) -> Tuple[str, float]:
    # Warm-up
    func()
    t0 = time.perf_counter()
    for _ in range(iterations):
        func()
    dt = time.perf_counter() - t0
    return label, (dt * 1e6) / iterations  # microseconds per call


def _bench_group(
    exprs: Iterable[Tuple[str, Callable[[], object]]], iterations: int
) -> List[Tuple[str, float]]:
    return [bench(label, fn, iterations=iterations) for label, fn in exprs]


def _default_sample_path() -> Path:
    root = Path(__file__).resolve().parents[2]
    legacy_name = (
        "1.2.840.113619.2.99.1234.1210123180.675655_0000_000034_121066021209fb.dcm"
    )
    legacy_candidates = [
        root.parent / "sample" / legacy_name,
        root / "sample" / legacy_name,
    ]
    for legacy in legacy_candidates:
        if legacy.exists():
            return legacy
    return root / "tests" / "test_le.dcm"


def run_dicomsdl(sample_path: Path, which: str = "both") -> List[Tuple[str, float]]:
    import dicomsdl as dicom

    ds = dicom.read_file(str(sample_path))
    sequence_ds = dicom.DataSet()
    sequence_ds.set_value(
        "RadiopharmaceuticalInformationSequence.0.RadionuclideHalfLife", 123.0
    )
    scalar_exprs = [
        ("dicomsdl ds.Rows", lambda: ds.Rows),
        ("dicomsdl ds.get_value(0x00280010)", lambda: ds.get_value(0x00280010)),
        ("dicomsdl ds.get_value('Rows')", lambda: ds.get_value("Rows")),
        ("dicomsdl ds['Rows'].value", lambda: ds["Rows"].value),
        ("dicomsdl ds[0x00280010].value", lambda: ds[0x00280010].value),
        ("dicomsdl ds[0x00280010]", lambda: ds[0x00280010]),
        ("dicomsdl ds['Rows']", lambda: ds["Rows"]),
        ("dicomsdl getde to_long", lambda: ds.get_dataelement(0x00280010).to_long()),
        ("dicomsdl getde get_value", lambda: ds.get_dataelement(0x00280010).get_value()),
    ]

    sequence_exprs = [
        ("dicomsdl packed path", lambda: sequence_ds["00540016.0.00181075"]),
        (
            "dicomsdl string path",
            lambda: sequence_ds["RadiopharmaceuticalInformationSequence.0.RadionuclideHalfLife"],
        ),
        (
            "dicomsdl string path value",
            lambda: sequence_ds["RadiopharmaceuticalInformationSequence.0.RadionuclideHalfLife"].value,
        ),
        (
            "dicomsdl get_value(path)",
            lambda: sequence_ds.get_value(
                "RadiopharmaceuticalInformationSequence.0.RadionuclideHalfLife"
            ),
        ),
        (
            "dicomsdl attr chain",
            lambda: sequence_ds.RadiopharmaceuticalInformationSequence[0].RadionuclideHalfLife,
        ),
        (
            "dicomsdl get_value chain",
            lambda: sequence_ds.get_dataelement(0x00540016)
            .sequence[0]
            .get_dataelement(0x00181075)
            .get_value(),
        ),
        (
            "dicomsdl to_long chain",
            lambda: sequence_ds.get_dataelement(0x00540016)
            .sequence[0]
            .get_dataelement(0x00181075)
            .to_long(),
        ),
    ]

    mutation_ds = dicom.DataSet()
    rows_elem = mutation_ds.ensure_dataelement("Rows", dicom.VR.US)
    rows_elem.value = 512
    path_elem = mutation_ds.ensure_dataelement(
        "ReferencedStudySequence.0.ReferencedSOPInstanceUID", dicom.VR.UI
    )
    path_elem.value = "1.2.3.4"

    mutation_flat_exprs = [
        ("dicomsdl elem.value = 512", lambda: setattr(rows_elem, "value", 512)),
        ("dicomsdl ds.set_value('Rows', 512)", lambda: mutation_ds.set_value("Rows", 512)),
        (
            "dicomsdl ensure('Rows').value = 512",
            lambda: setattr(mutation_ds.ensure_dataelement("Rows"), "value", 512),
        ),
    ]

    mutation_path_exprs = [
        (
            "dicomsdl path set_value",
            lambda: mutation_ds.set_value(
                "ReferencedStudySequence.0.ReferencedSOPInstanceUID", "1.2.3.4"
            ),
        ),
        (
            "dicomsdl path ensure().value",
            lambda: setattr(
                mutation_ds.ensure_dataelement(
                    "ReferencedStudySequence.0.ReferencedSOPInstanceUID", dicom.VR.UI
                ),
                "value",
                "1.2.3.4",
            ),
        ),
    ]

    results: List[Tuple[str, float]] = []
    if which in ("both", "all", "scalars"):
        results.extend(_bench_group(scalar_exprs, iterations=100_000))
    if which in ("both", "all", "sequence"):
        results.extend(_bench_group(sequence_exprs, iterations=5_000))
    if which in ("both", "all", "mutation"):
        results.extend(_bench_group(mutation_flat_exprs, iterations=50_000))
        results.extend(_bench_group(mutation_path_exprs, iterations=5_000))
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
    if which in ("both", "all", "scalars"):
        results.extend(_bench_group(scalar_exprs, iterations=100_000))
    return results


def main() -> int:
    if len(sys.argv) > 1:
        sample = Path(sys.argv[1]).expanduser().resolve()
        which = "both"
        if len(sys.argv) > 2:
            which = sys.argv[2]
    else:
        sample = _default_sample_path()
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
                "dicomsdl ds.get_value(0x00280010)",
                "dicomsdl ds.get_value('Rows')",
                "dicomsdl ds['Rows'].value",
                "dicomsdl ds[0x00280010].value",
                "dicomsdl ds[0x00280010]",
                "dicomsdl ds['Rows']",
                "dicomsdl getde to_long",
                "dicomsdl getde get_value",
            }
            sequence_labels = {
                "dicomsdl packed path",
                "dicomsdl string path",
                "dicomsdl string path value",
                "dicomsdl get_value(path)",
                "dicomsdl attr chain",
                "dicomsdl get_value chain",
                "dicomsdl to_long chain",
            }
            mutation_labels = {
                "dicomsdl elem.value = 512",
                "dicomsdl ds.set_value('Rows', 512)",
                "dicomsdl ensure('Rows').value = 512",
                "dicomsdl path set_value",
                "dicomsdl path ensure().value",
            }
            scalar_res = [(l, u) for l, u in dicomsdl_results if l in scalar_labels]
            seq_res = [(l, u) for l, u in dicomsdl_results if l in sequence_labels]
            mutation_res = [(l, u) for l, u in dicomsdl_results if l in mutation_labels]
            if scalar_res:
                print("  -- Scalars --")
                for label, us in sorted(scalar_res, key=lambda x: x[1]):
                    print(f"{label:40s} {us:6.2f} µs/call")
            if seq_res:
                print("  -- Sequence --")
                for label, us in sorted(seq_res, key=lambda x: x[1]):
                    print(f"{label:40s} {us:6.2f} µs/call")
            if mutation_res:
                print("  -- Mutation --")
                for label, us in sorted(mutation_res, key=lambda x: x[1]):
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
