# encoding=utf-8

import importlib as _importlib
import importlib.util as _importlib_util
import os as _os
import pathlib as _pathlib
import sys as _sys
import warnings as _warnings

_PACKAGE_DIR = _pathlib.Path(__file__).resolve().parent
_WINDOWS_DLL_DIRECTORY_HANDLES = []
_NATIVE_MODULE_PATTERNS = ("_dicomsdl*.so", "_dicomsdl*.pyd", "_dicomsdl*.dylib")


def _configure_windows_dll_search_path():
    if _sys.platform != "win32":
        return
    add_dll_directory = getattr(_os, "add_dll_directory", None)
    if add_dll_directory is None:
        return
    try:
        handle = add_dll_directory(str(_PACKAGE_DIR))
    except OSError:
        return
    _WINDOWS_DLL_DIRECTORY_HANDLES.append(handle)


def _codec_plugin_library_suffix():
    if _sys.platform == "win32":
        return ".dll"
    if _sys.platform == "darwin":
        return ".dylib"
    return ".so"


def _discover_native_module_path():
    explicit_path = _os.environ.get("DICOMSDL_NATIVE_MODULE_PATH", "").strip()
    if explicit_path:
        candidate = _pathlib.Path(explicit_path).expanduser().resolve()
        if candidate.is_file():
            return str(candidate)
        raise ModuleNotFoundError(
            f"DICOMSDL_NATIVE_MODULE_PATH does not point to a file: {candidate}"
        )

    search_roots = []
    seen_roots = set()
    for raw_entry in _sys.path:
        if not raw_entry:
            continue
        try:
            entry = _pathlib.Path(raw_entry).resolve()
        except OSError:
            continue
        if not entry.is_dir():
            continue
        key = str(entry)
        if key in seen_roots:
            continue
        seen_roots.add(key)
        search_roots.append(entry)

    candidates = []
    seen_candidates = set()

    def _append_candidate(path):
        try:
            resolved = path.resolve()
        except OSError:
            return
        if not resolved.is_file():
            return
        key = str(resolved)
        if key in seen_candidates:
            return
        seen_candidates.add(key)
        candidates.append(resolved)

    for root in search_roots:
        for pattern in _NATIVE_MODULE_PATTERNS:
            for path in sorted(root.glob(pattern)):
                _append_candidate(path)

    for root in search_roots:
        root_lc = str(root).lower()
        if "build" not in root_lc and "_deps" not in root_lc:
            continue
        for pattern in _NATIVE_MODULE_PATTERNS:
            for path in root.rglob(pattern):
                _append_candidate(path)

    if not candidates:
        return None

    candidates.sort(
        key=lambda path: (path.stat().st_mtime_ns, len(str(path))),
        reverse=True,
    )
    return str(candidates[0])


def _load_native_module():
    native_module_name = f"{__name__}._dicomsdl"

    cached = _sys.modules.get(native_module_name)
    if cached is not None:
        return cached

    # Normal installed layout: dicomsdl/_dicomsdl.* lives under this package.
    if _importlib_util.find_spec(native_module_name) is not None:
        return _importlib.import_module("._dicomsdl", __name__)

    # Dev/CI fallback: extension exists only as top-level _dicomsdl.* in build dir.
    top_level_spec = _importlib_util.find_spec("_dicomsdl")
    if top_level_spec is not None and top_level_spec.origin is not None:
        native_module_path = top_level_spec.origin
    else:
        native_module_path = _discover_native_module_path()
        if native_module_path is None:
            raise ModuleNotFoundError("Cannot find native module '_dicomsdl'")

    alias_spec = _importlib_util.spec_from_file_location(
        native_module_name, native_module_path
    )
    if alias_spec is None or alias_spec.loader is None:
        raise ImportError("Failed to create module spec for dicomsdl._dicomsdl")

    native = _importlib_util.module_from_spec(alias_spec)
    _sys.modules[native_module_name] = native
    _sys.modules.setdefault("_dicomsdl", native)
    alias_spec.loader.exec_module(native)
    return native


_configure_windows_dll_search_path()
_dicomsdl = _load_native_module()

_export_names = getattr(
    _dicomsdl,
    "__all__",
    tuple(name for name in vars(_dicomsdl) if not name.startswith("_")),
)
globals().update({name: getattr(_dicomsdl, name) for name in _export_names})


def bundled_codec_plugin_libraries():
    suffix = _codec_plugin_library_suffix()
    patterns = (
        f"dicomsdl_codec_*_plugin{suffix}",
        f"libdicomsdl_codec_*_plugin{suffix}",
    )
    entries = []
    for pattern in patterns:
        entries.extend(sorted(_PACKAGE_DIR.glob(pattern)))

    unique_paths = []
    seen = set()
    for path in entries:
        resolved = str(path.resolve())
        if resolved in seen:
            continue
        seen.add(resolved)
        unique_paths.append(resolved)
    return tuple(unique_paths)


def load_bundled_codec_plugins(*, strict=False):
    register_plugin = getattr(_dicomsdl, "register_external_codec_plugin", None)
    if register_plugin is None:
        if strict:
            raise RuntimeError(
                "Native module does not expose register_external_codec_plugin"
            )
        return tuple()

    loaded_plugin_keys = []
    for library_path in bundled_codec_plugin_libraries():
        try:
            plugin_key = register_plugin(library_path)
        except Exception as exc:  # pragma: no cover - exercised by integration setups
            if strict:
                raise
            _warnings.warn(
                f"Failed to load bundled codec plugin '{library_path}': {exc}",
                RuntimeWarning,
                stacklevel=2,
            )
            continue
        loaded_plugin_keys.append(plugin_key)
    return tuple(loaded_plugin_keys)


def _autoload_bundled_codec_plugins():
    value = _os.environ.get("DICOMSDL_AUTOLOAD_BUNDLED_CODECS", "1").strip().lower()
    if value in {"0", "false", "no", "off"}:
        return
    load_bundled_codec_plugins(strict=False)


from . import _image as _image  # noqa: F401

DICOM_STANDARD_VERSION = getattr(_dicomsdl, "DICOM_STANDARD_VERSION", "")
DICOMSDL_VERSION = getattr(_dicomsdl, "DICOMSDL_VERSION", "")
__version__ = getattr(_dicomsdl, "__version__", DICOMSDL_VERSION)

_autoload_bundled_codec_plugins()

__all__ = tuple(_export_names)
