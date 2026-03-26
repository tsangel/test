from __future__ import annotations

import sys
from pathlib import Path


def _find_repo_build_package_root(repo_root: Path) -> Path | None:
    build_dir = repo_root / "build"
    if not build_dir.is_dir():
        return None

    candidates = [
        lib_dir
        for lib_dir in build_dir.glob("lib.*")
        if (lib_dir / "dicomsdl" / "__init__.py").is_file()
    ]
    if not candidates:
        return None

    candidates.sort(
        key=lambda path: (path.stat().st_mtime_ns, len(str(path))),
        reverse=True,
    )
    return candidates[0]


def _extend_package_path(package, extra_dir: Path) -> None:
    extra_text = str(extra_dir)
    if extra_text not in package.__path__:
        package.__path__.append(extra_text)
    spec = getattr(package, "__spec__", None)
    search_locations = getattr(spec, "submodule_search_locations", None)
    if search_locations is not None and extra_text not in search_locations:
        search_locations.append(extra_text)


def configure_repo_dicomsdl_import() -> bool:
    repo_root = Path(__file__).resolve().parents[3]
    bridge_package_dir = repo_root / "bindings" / "python" / "dicomsdl"
    if not bridge_package_dir.is_dir():
        return False

    try:
        import dicomsdl as package
    except ImportError:
        build_package_root = _find_repo_build_package_root(repo_root)
        if build_package_root is None:
            return False
        build_path = str(build_package_root)
        if build_path not in sys.path:
            sys.path.insert(0, build_path)
        import dicomsdl as package

    _extend_package_path(package, bridge_package_dir)
    return True


__all__ = ("configure_repo_dicomsdl_import",)
