from . import _dicomsdl as _dicomsdl
from ._dicomsdl import *

def bundled_codec_plugin_libraries() -> tuple[str, ...]: ...
def load_bundled_codec_plugins(*, strict: bool = ...) -> tuple[str, ...]: ...

__all__: tuple[str, ...]
