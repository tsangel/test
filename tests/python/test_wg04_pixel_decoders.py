from __future__ import annotations

import os
from pathlib import Path

import dicomsdl as dicom
import pytest

np = pytest.importorskip("numpy")

_DEFAULT_WG04_IMAGES_ROOT = Path("/Users/tsangel/workspace.dev/sample/nema/WG04/IMAGES")
_WG04_IMAGES_ENV = "DICOMSDL_WG04_IMAGES_BASE"


def _images_root() -> Path:
    override = os.environ.get(_WG04_IMAGES_ENV)
    if override:
        return Path(override)
    return _DEFAULT_WG04_IMAGES_ROOT


_WG04_IMAGES_ROOT = _images_root()
if not _WG04_IMAGES_ROOT.exists():
    pytest.skip(
        f"WG04 sample images not found at '{_WG04_IMAGES_ROOT}' "
        f"(override with {_WG04_IMAGES_ENV})",
        allow_module_level=True,
    )

_REF_DIR = _WG04_IMAGES_ROOT / "REF"
_RLE_DIR = _WG04_IMAGES_ROOT / "RLE"
_J2KR_DIR = _WG04_IMAGES_ROOT / "J2KR"
_J2KI_DIR = _WG04_IMAGES_ROOT / "J2KI"
_JLSL_DIR = _WG04_IMAGES_ROOT / "JLSL"
_JLSN_DIR = _WG04_IMAGES_ROOT / "JLSN"
_JPLL_DIR = _WG04_IMAGES_ROOT / "JPLL"
_JPLY_DIR = _WG04_IMAGES_ROOT / "JPLY"


def _series_key(path: Path) -> str:
    return path.name.split("_", 1)[0]


def _files(directory: Path) -> list[Path]:
    files = sorted(p for p in directory.iterdir() if p.is_file())
    if not files:
        pytest.fail(f"expected at least one file in {directory}")
    return files


def _reference_map() -> dict[str, Path]:
    return {_series_key(path): path for path in _files(_REF_DIR)}


def _pairs(compressed_dir: Path) -> list[tuple[Path, Path]]:
    ref_map = _reference_map()
    pairs: list[tuple[Path, Path]] = []
    missing_refs: list[str] = []
    for path in _files(compressed_dir):
        ref = ref_map.get(_series_key(path))
        if ref is None:
            missing_refs.append(path.name)
            continue
        pairs.append((path, ref))
    if missing_refs:
        pytest.fail(
            f"missing REF pair(s) for {compressed_dir.name}: {', '.join(sorted(missing_refs))}"
        )
    return pairs


_REF_FILES = _files(_REF_DIR)
_RLE_PAIRS = _pairs(_RLE_DIR)
_J2KR_PAIRS = _pairs(_J2KR_DIR)
_J2KI_PAIRS = _pairs(_J2KI_DIR)
_JLSL_PAIRS = _pairs(_JLSL_DIR)
_JLSN_PAIRS = _pairs(_JLSN_DIR)
_JPLL_PAIRS = _pairs(_JPLL_DIR)
_JPLY_PAIRS = _pairs(_JPLY_DIR)
_RLE_IDS = [compressed.name for compressed, _ in _RLE_PAIRS]
_J2KR_IDS = [compressed.name for compressed, _ in _J2KR_PAIRS]
_J2KI_IDS = [compressed.name for compressed, _ in _J2KI_PAIRS]
_JLSL_IDS = [compressed.name for compressed, _ in _JLSL_PAIRS]
_JLSN_IDS = [compressed.name for compressed, _ in _JLSN_PAIRS]
_JPLL_IDS = [compressed.name for compressed, _ in _JPLL_PAIRS]
_JPLY_IDS = [compressed.name for compressed, _ in _JPLY_PAIRS]

_J2KI_MAX_MAE = 55.0
_JLSN_MAX_ABS_ERROR = 10.0
_JPLY_MAX_MAE = 60.0


def _decode(path: Path):
    dicom_file = dicom.read_file(str(path))
    return dicom_file.to_array()


def _assert_same_pixels(decoded, reference, source_name: str):
    assert decoded.shape == reference.shape, (
        f"{source_name}: shape mismatch decoded={decoded.shape} reference={reference.shape}"
    )
    assert decoded.dtype == reference.dtype, (
        f"{source_name}: dtype mismatch decoded={decoded.dtype} reference={reference.dtype}"
    )
    assert np.array_equal(decoded, reference), f"{source_name}: pixel data mismatch"


def _mae(decoded, reference) -> float:
    diff = decoded.astype(np.float64) - reference.astype(np.float64)
    return float(np.mean(np.abs(diff)))


def _max_abs_error(decoded, reference) -> float:
    diff = decoded.astype(np.float64) - reference.astype(np.float64)
    return float(np.max(np.abs(diff)))


@pytest.mark.parametrize("ref_path", _REF_FILES, ids=lambda p: p.name)
def test_wg04_ref_decode_smoke(ref_path: Path):
    arr = _decode(ref_path)
    assert arr.size > 0


@pytest.mark.parametrize("rle_path,ref_path", _RLE_PAIRS, ids=_RLE_IDS)
def test_wg04_rle_matches_ref(rle_path: Path, ref_path: Path):
    decoded = _decode(rle_path)
    reference = _decode(ref_path)
    _assert_same_pixels(decoded, reference, rle_path.name)


@pytest.mark.parametrize("j2kr_path,ref_path", _J2KR_PAIRS, ids=_J2KR_IDS)
def test_wg04_j2kr_matches_ref(j2kr_path: Path, ref_path: Path):
    decoded = _decode(j2kr_path)
    reference = _decode(ref_path)
    _assert_same_pixels(decoded, reference, j2kr_path.name)


@pytest.mark.parametrize("j2ki_path,ref_path", _J2KI_PAIRS, ids=_J2KI_IDS)
def test_wg04_j2ki_matches_ref_with_mae_tolerance(j2ki_path: Path, ref_path: Path):
    decoded = _decode(j2ki_path)
    reference = _decode(ref_path)

    assert decoded.shape == reference.shape, (
        f"{j2ki_path.name}: shape mismatch decoded={decoded.shape} reference={reference.shape}"
    )
    assert decoded.dtype == reference.dtype, (
        f"{j2ki_path.name}: dtype mismatch decoded={decoded.dtype} reference={reference.dtype}"
    )

    mae = _mae(decoded, reference)
    assert mae <= _J2KI_MAX_MAE, (
        f"{j2ki_path.name}: MAE too high ({mae:.3f} > {_J2KI_MAX_MAE:.3f})"
    )


@pytest.mark.parametrize("jlsl_path,ref_path", _JLSL_PAIRS, ids=_JLSL_IDS)
def test_wg04_jlsl_matches_ref(jlsl_path: Path, ref_path: Path):
    decoded = _decode(jlsl_path)
    reference = _decode(ref_path)
    _assert_same_pixels(decoded, reference, jlsl_path.name)


@pytest.mark.parametrize("jlsn_path,ref_path", _JLSN_PAIRS, ids=_JLSN_IDS)
def test_wg04_jlsn_matches_ref_with_max_abs_tolerance(jlsn_path: Path, ref_path: Path):
    decoded = _decode(jlsn_path)
    reference = _decode(ref_path)

    assert decoded.shape == reference.shape, (
        f"{jlsn_path.name}: shape mismatch decoded={decoded.shape} reference={reference.shape}"
    )
    assert decoded.dtype == reference.dtype, (
        f"{jlsn_path.name}: dtype mismatch decoded={decoded.dtype} reference={reference.dtype}"
    )

    max_abs_error = _max_abs_error(decoded, reference)
    assert max_abs_error <= _JLSN_MAX_ABS_ERROR, (
        f"{jlsn_path.name}: max abs error too high "
        f"({max_abs_error:.3f} > {_JLSN_MAX_ABS_ERROR:.3f})"
    )


@pytest.mark.parametrize("jpll_path,ref_path", _JPLL_PAIRS, ids=_JPLL_IDS)
def test_wg04_jpll_matches_ref(jpll_path: Path, ref_path: Path):
    decoded = _decode(jpll_path)
    reference = _decode(ref_path)
    _assert_same_pixels(decoded, reference, jpll_path.name)


@pytest.mark.parametrize("jply_path,ref_path", _JPLY_PAIRS, ids=_JPLY_IDS)
def test_wg04_jply_matches_ref_with_mae_tolerance(jply_path: Path, ref_path: Path):
    decoded = _decode(jply_path)
    reference = _decode(ref_path)

    assert decoded.shape == reference.shape, (
        f"{jply_path.name}: shape mismatch decoded={decoded.shape} reference={reference.shape}"
    )
    if decoded.dtype != reference.dtype:
        # Some WG04 JPLY samples report unsigned metadata while REF uses signed int16.
        assert decoded.dtype == np.uint16 and reference.dtype == np.int16, (
            f"{jply_path.name}: dtype mismatch decoded={decoded.dtype} reference={reference.dtype}"
        )

    mae = _mae(decoded, reference)
    assert mae <= _JPLY_MAX_MAE, (
        f"{jply_path.name}: MAE too high ({mae:.3f} > {_JPLY_MAX_MAE:.3f})"
    )
