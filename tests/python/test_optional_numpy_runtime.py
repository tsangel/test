from __future__ import annotations

import subprocess
import sys
from pathlib import Path


def _find_native_module(root: Path) -> Path | None:
    candidates: list[Path] = []
    for build_dir in root.glob("build*"):
        if not build_dir.is_dir():
            continue
        candidates.extend(build_dir.rglob("_dicomsdl*.pyd"))
        candidates.extend(build_dir.rglob("_dicomsdl*.so"))
        candidates.extend(build_dir.rglob("_dicomsdl*.dylib"))
    if not candidates:
        return None
    candidates.sort(key=lambda path: (path.stat().st_mtime_ns, len(str(path))), reverse=True)
    return candidates[0]


def test_import_and_transcode_surface_available_without_numpy():
    root = Path(__file__).resolve().parents[2]
    python_paths = []
    repo_package_dir = root / "bindings/python"
    native_module = _find_native_module(root)
    if repo_package_dir.is_dir():
        python_paths.append(str(repo_package_dir))
    if native_module is not None:
        python_paths.append(str(native_module.parent))

    env = None
    if python_paths:
        env = dict(**__import__("os").environ)
        existing = env.get("PYTHONPATH", "")
        env["PYTHONPATH"] = (
            __import__("os").pathsep.join([*python_paths, existing]) if existing else __import__("os").pathsep.join(python_paths)
        )
        if native_module is not None:
            env["DICOMSDL_NATIVE_MODULE_PATH"] = str(native_module)

    code = r"""
import builtins

real_import = builtins.__import__

def blocked_import(name, globals=None, locals=None, fromlist=(), level=0):
    if name == "numpy" or name.startswith("numpy."):
        raise ImportError("blocked numpy for test")
    return real_import(name, globals, locals, fromlist, level)

builtins.__import__ = blocked_import

import dicomsdl as dicom
from dicomsdl import dicomconv

assert hasattr(dicom, "read_file")
assert hasattr(dicom, "is_dicom_file")
assert hasattr(dicom.DicomFile, "set_transfer_syntax")
assert hasattr(dicom.DicomFile, "write_file")
assert callable(dicomconv.build_parser)
dicomconv.build_parser()
print("ok")
"""
    result = subprocess.run(
        [sys.executable, "-c", code],
        check=False,
        capture_output=True,
        text=True,
        env=env,
    )
    assert result.returncode == 0, result.stderr or result.stdout
