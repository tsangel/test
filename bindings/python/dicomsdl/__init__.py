# encoding=utf-8

from . import _dicomsdl as _dicomsdl  # noqa: F401
from ._dicomsdl import *  # noqa: F401,F403

DICOM_STANDARD_VERSION = getattr(_dicomsdl, "DICOM_STANDARD_VERSION", "")
DICOMSDL_VERSION = getattr(_dicomsdl, "DICOMSDL_VERSION", "")
__version__ = getattr(_dicomsdl, "__version__", DICOMSDL_VERSION)

__all__ = getattr(
    _dicomsdl,
    "__all__",
    tuple(name for name in globals() if not name.startswith("_")),
)
