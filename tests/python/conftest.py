from __future__ import annotations

import os
import pathlib
import sys
import sysconfig

import pytest


ROOT = pathlib.Path(__file__).resolve().parents[2]
BINDINGS_DIR = ROOT / "bindings" / "python"
BUILD_GLOBS = ("build*",)
NATIVE_PATTERNS = ("_dicomsdl*.pyd", "_dicomsdl*.so", "_dicomsdl*.dylib")


def _explicit_native_module() -> pathlib.Path | None:
    raw = os.environ.get("DICOMSDL_NATIVE_MODULE_PATH", "").strip()
    if not raw:
        return None
    candidate = pathlib.Path(raw).expanduser().resolve()
    if candidate.is_file():
        return candidate
    return None


def _current_extension_suffix() -> str:
    return (sysconfig.get_config_var("EXT_SUFFIX") or "").strip()


def _candidate_priority(path: pathlib.Path) -> tuple[bool, int, int]:
    current_suffix = _current_extension_suffix()
    suffix_matches = bool(current_suffix) and path.name.endswith(current_suffix)
    return (suffix_matches, path.stat().st_mtime_ns, len(str(path)))


def _find_native_module() -> pathlib.Path | None:
    explicit = _explicit_native_module()
    if explicit is not None:
        return explicit

    candidates: list[pathlib.Path] = []
    for build_glob in BUILD_GLOBS:
        for build_dir in ROOT.glob(build_glob):
            if not build_dir.is_dir():
                continue
            for pattern in NATIVE_PATTERNS:
                candidates.extend(build_dir.rglob(pattern))
    if not candidates:
        return None
    candidates.sort(key=_candidate_priority, reverse=True)
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
            compiler_path = pathlib.Path(line[len(prefix):].strip())
            if compiler_path.exists():
                return compiler_path.parent
    return None


def _configure_sys_path(native_module: pathlib.Path) -> None:
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
        build_root / "_deps" / "dicomsdl_libjpeg_turbo-build" / "sharedlib" if build_root else None,
        build_root / "_deps" / "dicomsdl_libdeflate-build" if build_root else None,
    ]
    for path in candidate_dirs:
        if path is not None and path.exists():
            add_dll_directory(str(path))


def pytest_configure(config: pytest.Config) -> None:
    native_module = _find_native_module()
    if native_module is None:
        raise pytest.UsageError(
            "Cannot find built native module '_dicomsdl'. Build the project first, "
            "for example with 'cmake --build build-msyscheck --target _dicomsdl'."
        )

    _configure_windows_dll_dirs(native_module)
    _configure_sys_path(native_module)
    os.environ["DICOMSDL_NATIVE_MODULE_PATH"] = str(native_module)
