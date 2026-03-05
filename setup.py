import os
import pathlib
import re
import shlex
import shutil
import subprocess
import sys
import sysconfig
from typing import Optional
from setuptools import Extension, setup
from setuptools.command.build_ext import build_ext

ROOT_DIR = pathlib.Path(__file__).resolve().parent
VERSION_FILE = ROOT_DIR / "VERSION"
CODEC_KEYS = ("JPEG", "JPEGLS", "JPEG2K", "HTJ2K", "JPEGXL")
CODEC_SHARED_PLUGIN_TARGET = {
    "JPEG": "dicomsdl_pixel_jpeg_plugin",
    "JPEGLS": "dicomsdl_pixel_jpegls_plugin",
    "JPEG2K": "dicomsdl_pixel_openjpeg_plugin",
    "HTJ2K": "dicomsdl_pixel_htj2k_plugin",
    "JPEGXL": "dicomsdl_pixel_jpegxl_plugin",
}
CODEC_PIXEL_PLUGIN_OPTIONS = {
    "JPEG": ("DICOMSDL_PIXEL_JPEG_STATIC_PLUGIN", "DICOMSDL_PIXEL_JPEG_PLUGIN"),
    "JPEGLS": ("DICOMSDL_PIXEL_JPEGLS_STATIC_PLUGIN", "DICOMSDL_PIXEL_JPEGLS_PLUGIN"),
    "JPEG2K": ("DICOMSDL_PIXEL_OPENJPEG_STATIC_PLUGIN", "DICOMSDL_PIXEL_OPENJPEG_PLUGIN"),
    "HTJ2K": ("DICOMSDL_PIXEL_HTJ2K_STATIC_PLUGIN", "DICOMSDL_PIXEL_HTJ2K_PLUGIN"),
    "JPEGXL": ("DICOMSDL_PIXEL_JPEGXL_STATIC_PLUGIN", "DICOMSDL_PIXEL_JPEGXL_PLUGIN"),
}
VALID_CODEC_MODES = {"builtin", "shared", "none"}
TRUTHY_ENV_VALUES = {"1", "true", "yes", "on"}

def load_version() -> str:
    text = VERSION_FILE.read_text(encoding="utf-8").strip()
    if not text:
        raise RuntimeError(f"Version file {VERSION_FILE} is empty")
    return text

PACKAGE_VERSION = load_version()


def normalize_codec_mode(value: str, var_name: str) -> str:
    mode = value.strip().lower()
    if mode not in VALID_CODEC_MODES:
        allowed = ", ".join(sorted(VALID_CODEC_MODES))
        raise RuntimeError(f"{var_name} must be one of {allowed} (got: {value!r})")
    return mode


def resolve_codec_modes_from_env() -> dict[str, str]:
    default_mode = normalize_codec_mode(
        os.environ.get("DICOMSDL_PIXEL_DEFAULT_MODE", "builtin"),
        "DICOMSDL_PIXEL_DEFAULT_MODE",
    )
    modes: dict[str, str] = {}
    for codec_key in CODEC_KEYS:
        mode_var = f"DICOMSDL_PIXEL_{codec_key}_MODE"
        # Keep JPEG XL opt-in by default to avoid forcing libjxl dependency.
        fallback_mode = "none" if codec_key == "JPEGXL" else default_mode
        mode_value = os.environ.get(mode_var, fallback_mode)
        modes[codec_key] = normalize_codec_mode(mode_value, mode_var)
    return modes


def dynamic_library_suffixes() -> tuple[str, ...]:
    if sys.platform == "win32":
        return (".dll",)
    if sys.platform == "darwin":
        return (".dylib",)
    return (".so",)


def parse_env_bool(var_name: str, default: bool) -> bool:
    raw = os.environ.get(var_name)
    if raw is None:
        return default
    return raw.strip().lower() in TRUTHY_ENV_VALUES


def resolve_strip_command() -> Optional[list[str]]:
    custom = os.environ.get("DICOMSDL_STRIP_TOOL", "").strip()
    if custom:
        parts = shlex.split(custom)
        return parts if parts else None

    for tool_name in ("strip", "llvm-strip"):
        tool_path = shutil.which(tool_name)
        if tool_path:
            return [tool_path]
    return None


def resolve_strip_args() -> list[str]:
    custom = os.environ.get("DICOMSDL_STRIP_ARGS", "").strip()
    if custom:
        return shlex.split(custom)
    if sys.platform == "win32":
        # PE/COFF builds usually use debug symbol stripping semantics.
        return ["--strip-debug"]
    return ["-x"]


def maybe_strip_binary_artifacts(paths: list[pathlib.Path]) -> None:
    if not parse_env_bool("DICOMSDL_STRIP_BINARIES", True):
        return

    strip_cmd = resolve_strip_command()
    if strip_cmd is None:
        print("warning: strip skipped (no strip tool found)")
        return

    strip_args = resolve_strip_args()
    required = parse_env_bool("DICOMSDL_STRIP_REQUIRED", False)
    seen: set[pathlib.Path] = set()

    for path in paths:
        if not path.exists() or not path.is_file():
            continue
        resolved = path.resolve()
        if resolved in seen:
            continue
        seen.add(resolved)

        cmd = [*strip_cmd, *strip_args, str(resolved)]
        try:
            subprocess.check_call(cmd)
        except Exception as exc:
            message = f"warning: strip failed for {resolved}: {exc}"
            if required:
                raise RuntimeError(message) from exc
            print(message)


def find_dynamic_library(build_root: pathlib.Path, stem: str) -> pathlib.Path:
    candidates: list[pathlib.Path] = []
    for suffix in dynamic_library_suffixes():
        for prefix in ("", "lib"):
            name = f"{prefix}{stem}{suffix}"
            candidates.extend(path for path in build_root.rglob(name) if path.is_file())

    if not candidates:
        raise RuntimeError(
            f"Expected dynamic library for '{stem}' was not produced under {build_root}"
        )

    # Prefer the newest artifact when multiple config directories exist.
    candidates.sort(
        key=lambda path: (path.stat().st_mtime_ns, len(str(path))),
        reverse=True,
    )
    return candidates[0]


def remove_stale_shared_artifacts(extdir: pathlib.Path) -> None:
    for suffix in dynamic_library_suffixes():
        patterns = (
            f"dicomsdl_codec_runtime*{suffix}",
            f"libdicomsdl_codec_runtime*{suffix}",
            f"dicomsdl_codec_*_plugin*{suffix}",
            f"libdicomsdl_codec_*_plugin*{suffix}",
            f"dicomsdl_pixel_*_plugin*{suffix}",
            f"libdicomsdl_pixel_*_plugin*{suffix}",
        )
        for pattern in patterns:
            for path in extdir.glob(pattern):
                if path.is_file():
                    path.unlink()


class CMakeExtension(Extension):
    def __init__(self, name: str, sourcedir: str = "") -> None:
        super().__init__(name, sources=[])
        self.sourcedir = os.path.abspath(sourcedir) if sourcedir else str(ROOT_DIR)


class CMakeBuild(build_ext):
    def build_extension(self, ext: Extension) -> None:
        ext_full_path = pathlib.Path(self.get_ext_fullpath(ext.name)).resolve()
        extdir = ext_full_path.parent
        extdir.mkdir(parents=True, exist_ok=True)
        remove_stale_shared_artifacts(extdir)

        cfg = "Debug" if self.debug else "Release"

        python_executable = sys.executable
        base_executable = getattr(sys, "_base_executable", python_executable)
        if sys.prefix != sys.base_prefix and base_executable:
            python_executable = base_executable
        python_root_dir = pathlib.Path(python_executable).resolve().parent.parent
        python_tag = sysconfig.get_python_version().replace(".", "")
        plat_name_raw = getattr(self, "plat_name", sysconfig.get_platform())
        plat_name = plat_name_raw.replace("-", "_")
        build_suffix = f"{ext.name.replace('.', '_')}_{python_tag}_{plat_name}"

        cmake_args = [
            f"-DCMAKE_LIBRARY_OUTPUT_DIRECTORY={extdir}",
            f"-DPYTHON_EXECUTABLE={python_executable}",
            f"-DPython_EXECUTABLE={python_executable}",
            f"-DPython3_EXECUTABLE={python_executable}",
            f"-DPython_ROOT_DIR={python_root_dir}",
            f"-DPython3_ROOT_DIR={python_root_dir}",
            "-DDICOM_BUILD_PYTHON=ON",
            "-DDICOMSDL_PIXEL_CORE=ON",
            "-DDICOMSDL_PIXEL_RUNTIME=ON",
            "-DDICOMSDL_PIXEL_RLE_STATIC_PLUGIN=ON",
        ]

        codec_modes = resolve_codec_modes_from_env()
        shared_plugin_targets: list[str] = []
        jpegxl_mode_requested = False
        for codec_key in CODEC_KEYS:
            mode = codec_modes[codec_key]
            static_opt, shared_opt = CODEC_PIXEL_PLUGIN_OPTIONS[codec_key]
            if mode == "builtin":
                static_on = "ON"
                shared_on = "OFF"
            elif mode == "shared":
                static_on = "OFF"
                shared_on = "ON"
                shared_plugin_targets.append(CODEC_SHARED_PLUGIN_TARGET[codec_key])
            else:
                static_on = "OFF"
                shared_on = "OFF"
            cmake_args += [
                f"-D{static_opt}={static_on}",
                f"-D{shared_opt}={shared_on}",
            ]
            if codec_key == "JPEGXL" and mode != "none":
                jpegxl_mode_requested = True

        if jpegxl_mode_requested:
            cmake_args.append("-DDICOMSDL_ENABLE_JPEGXL=ON")

        extra_cmake_args = os.environ.get("DICOMSDL_CMAKE_ARGS") or os.environ.get("CMAKE_ARGS")
        if extra_cmake_args:
            cmake_args += shlex.split(extra_cmake_args)

        build_targets = ["_dicomsdl"]
        if shared_plugin_targets:
            build_targets += shared_plugin_targets
        build_args = ["--target", *build_targets]

        if self.compiler.compiler_type == "msvc":
            cmake_args += [f"-DCMAKE_LIBRARY_OUTPUT_DIRECTORY_{cfg.upper()}={extdir}"]
            build_args += ["--config", cfg]
        else:
            cmake_args += [f"-DCMAKE_BUILD_TYPE={cfg}"]

        if sys.platform == "darwin":
            arch_candidate = None
            plat_tokens = re.split(r"[-_]", plat_name_raw)
            if plat_tokens:
                maybe_arch = plat_tokens[-1]
                if maybe_arch in {"arm64", "x86_64"}:
                    arch_candidate = maybe_arch
            if arch_candidate:
                cmake_args += [f"-DCMAKE_OSX_ARCHITECTURES={arch_candidate}"]

        build_temp = pathlib.Path(self.build_temp) / build_suffix
        if build_temp.exists() and not self.force:
            shutil.rmtree(build_temp)
        build_temp.mkdir(parents=True, exist_ok=True)

        subprocess.check_call(
            ["cmake", "-S", ext.sourcedir, "-B", str(build_temp)] + cmake_args # type: ignore
        )
        subprocess.check_call(["cmake", "--build", str(build_temp)] + build_args)

        strip_targets: list[pathlib.Path] = []

        if shared_plugin_targets:
            for plugin_target in shared_plugin_targets:
                plugin_src = find_dynamic_library(build_temp, plugin_target)
                plugin_dst = extdir / plugin_src.name
                shutil.copy2(plugin_src, plugin_dst)
                strip_targets.append(plugin_dst)

        if not ext_full_path.exists():
            raise RuntimeError(f"Expected extension at {ext_full_path} not found")

        strip_targets.append(ext_full_path)
        maybe_strip_binary_artifacts(strip_targets)


setup(
    name="dicomsdl",
    version=PACKAGE_VERSION,
    author="DicomProject Authors",
    description="Python bindings for dicomsdl",
    python_requires=">=3.9",
    packages=["dicomsdl"],
    package_dir={"dicomsdl": "bindings/python/dicomsdl"},
    package_data={"dicomsdl": ["py.typed", "*.pyi", "*.dll", "*.so", "*.dylib"]},
    ext_modules=[CMakeExtension("dicomsdl._dicomsdl")],
    cmdclass={"build_ext": CMakeBuild},
    entry_points={
        "console_scripts": [
            "dicomdump=dicomsdl.dicomdump:main",
            "dicomconv=dicomsdl.dicomconv:main",
            "dicomshow=dicomsdl.dicomshow:main",
        ],
    },
    include_package_data=True,
    zip_safe=False,
)
