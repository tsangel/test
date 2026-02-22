# encoding=utf-8

import importlib as _importlib
import importlib.util as _importlib_util
import sys as _sys

_native_module_name = f"{__name__}._dicomsdl"
if _native_module_name in _sys.modules:
    _dicomsdl = _sys.modules[_native_module_name]
elif _importlib_util.find_spec(_native_module_name) is not None:
    _dicomsdl = _importlib.import_module("._dicomsdl", __name__)
else:
    _dicomsdl = _importlib.import_module("_dicomsdl")
    _sys.modules.setdefault(_native_module_name, _dicomsdl)

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
