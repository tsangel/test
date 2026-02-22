# encoding=utf-8

import importlib as _importlib
import importlib.util as _importlib_util
import sys as _sys

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
    if top_level_spec is None or top_level_spec.origin is None:
        raise ModuleNotFoundError("Cannot find native module '_dicomsdl'")

    alias_spec = _importlib_util.spec_from_file_location(
        native_module_name, top_level_spec.origin
    )
    if alias_spec is None or alias_spec.loader is None:
        raise ImportError("Failed to create module spec for dicomsdl._dicomsdl")

    native = _importlib_util.module_from_spec(alias_spec)
    _sys.modules[native_module_name] = native
    _sys.modules.setdefault("_dicomsdl", native)
    alias_spec.loader.exec_module(native)
    return native


_dicomsdl = _load_native_module()

_export_names = getattr(
    _dicomsdl,
    "__all__",
    tuple(name for name in vars(_dicomsdl) if not name.startswith("_")),
)
globals().update({name: getattr(_dicomsdl, name) for name in _export_names})

from . import _image as _image  # noqa: F401

DICOM_STANDARD_VERSION = getattr(_dicomsdl, "DICOM_STANDARD_VERSION", "")
DICOMSDL_VERSION = getattr(_dicomsdl, "DICOMSDL_VERSION", "")
__version__ = getattr(_dicomsdl, "__version__", DICOMSDL_VERSION)

__all__ = tuple(_export_names)
