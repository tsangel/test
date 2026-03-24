from __future__ import annotations

import subprocess
import sys
from pathlib import Path


def test_import_and_transcode_surface_available_without_numpy():
    root = Path(__file__).resolve().parents[2]
    python_paths = []
    repo_package_dir = root / "bindings/python"
    repo_build_dir = root / "build/_deps/dicomsdl_openjpeg-build/bin"
    if repo_package_dir.is_dir():
        python_paths.append(str(repo_package_dir))
    if repo_build_dir.is_dir():
        python_paths.append(str(repo_build_dir))

    env = None
    if python_paths:
        env = dict(**__import__("os").environ)
        existing = env.get("PYTHONPATH", "")
        env["PYTHONPATH"] = (
            __import__("os").pathsep.join([*python_paths, existing]) if existing else __import__("os").pathsep.join(python_paths)
        )

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
