import os
import pathlib
import re
import shlex
import shutil
import subprocess
import sys
import sysconfig
from setuptools import Extension, setup
from setuptools.command.build_ext import build_ext

ROOT_DIR = pathlib.Path(__file__).resolve().parent
VERSION_FILE = ROOT_DIR / "VERSION"
CODEC_KEYS = ("JPEG", "JPEGLS", "JPEG2K", "HTJ2K", "JPEGXL")
CODEC_TARGET_SUFFIX = {
    "JPEG": "jpeg",
    "JPEGLS": "jpegls",
    "JPEG2K": "jpeg2k",
    "HTJ2K": "htj2k",
    "JPEGXL": "jpegxl",
}
VALID_CODEC_MODES = {"builtin", "shared", "none"}

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
        os.environ.get("DICOMSDL_CODEC_DEFAULT_MODE", "builtin"),
        "DICOMSDL_CODEC_DEFAULT_MODE",
    )
    modes: dict[str, str] = {}
    for codec_key in CODEC_KEYS:
        mode_var = f"DICOMSDL_CODEC_{codec_key}_MODE"
        mode_value = os.environ.get(mode_var, default_mode)
        modes[codec_key] = normalize_codec_mode(mode_value, mode_var)
    return modes


def dynamic_library_suffixes() -> tuple[str, ...]:
    if sys.platform == "win32":
        return (".dll",)
    if sys.platform == "darwin":
        return (".dylib",)
    return (".so",)


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
        ]

        codec_modes = resolve_codec_modes_from_env()
        shared_codec_suffixes: list[str] = []
        for codec_key in CODEC_KEYS:
            mode = codec_modes[codec_key]
            builtin_on = "ON" if mode == "builtin" else "OFF"
            shared_on = "ON" if mode == "shared" else "OFF"
            cmake_args += [
                f"-DDICOMSDL_CODEC_{codec_key}_BUILTIN={builtin_on}",
                f"-DDICOMSDL_CODEC_{codec_key}_SHARED={shared_on}",
            ]
            if mode == "shared":
                shared_codec_suffixes.append(CODEC_TARGET_SUFFIX[codec_key])

        extra_cmake_args = os.environ.get("DICOMSDL_CMAKE_ARGS") or os.environ.get("CMAKE_ARGS")
        if extra_cmake_args:
            cmake_args += shlex.split(extra_cmake_args)

        build_targets = ["_dicomsdl"]
        if shared_codec_suffixes:
            build_targets.append("dicomsdl_codec_runtime")
            for suffix in shared_codec_suffixes:
                build_targets.append(f"dicomsdl_codec_plugin_{suffix}")
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

        if shared_codec_suffixes:
            runtime_src = find_dynamic_library(build_temp, "dicomsdl_codec_runtime")
            shutil.copy2(runtime_src, extdir / runtime_src.name)
            for suffix in shared_codec_suffixes:
                plugin_src = find_dynamic_library(
                    build_temp, f"dicomsdl_codec_{suffix}_plugin"
                )
                shutil.copy2(plugin_src, extdir / plugin_src.name)

        if not ext_full_path.exists():
            raise RuntimeError(f"Expected extension at {ext_full_path} not found")


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
