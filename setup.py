import os
import pathlib
import platform
import re
import shlex
import shutil
import subprocess
import sys
import sysconfig
from typing import Optional
from setuptools import Extension, setup
from setuptools.command.build_ext import build_ext

try:
    from setuptools.command.bdist_wheel import bdist_wheel
except ImportError:
    bdist_wheel = None

ROOT_DIR = pathlib.Path(__file__).resolve().parent
VERSION_HEADER_FILE = ROOT_DIR / "include" / "dicom_const.h"
VERSION_EXTRACTOR_SCRIPT = ROOT_DIR / "scripts" / "extract_version_from_const_header.py"
README_FILE = ROOT_DIR / "README.md"
TEMP_VERSION_FILE = pathlib.Path(
    os.environ.get(
        "DICOMSDL_TEMP_VERSION_FILE",
        str(ROOT_DIR / "build" / ".version_cache" / "VERSION"),
    )
).resolve()
TEMP_DICOM_STANDARD_VERSION_FILE = pathlib.Path(
    os.environ.get(
        "DICOM_STANDARD_TEMP_VERSION_FILE",
        str(ROOT_DIR / "build" / ".version_cache" / "DICOM_STANDARD_VERSION"),
    )
).resolve()
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
PROJECT_URL = "https://github.com/tsangel/dicomsdl"
PYPI_URL = "https://pypi.org/project/dicomsdl/"
PROJECT_URLS = {
    "Homepage": PROJECT_URL,
    "PyPI": PYPI_URL,
    "Source": PROJECT_URL,
    "Issues": f"{PROJECT_URL}/issues",
}
README_TEXT = README_FILE.read_text(encoding="utf-8")


def extract_macos_architectures(hint: str) -> set[str]:
    text = hint.strip().lower()
    if not text:
        return set()

    archs: set[str] = set()
    if "universal2" in text:
        archs.update({"arm64", "x86_64"})
    if "arm64" in text or "aarch64" in text:
        archs.add("arm64")
    if "x86_64" in text or "amd64" in text:
        archs.add("x86_64")
    return archs


def infer_macos_architecture(plat_name_hint: Optional[str] = None) -> str:
    archs: set[str] = set()
    hints = (
        os.environ.get("CMAKE_OSX_ARCHITECTURES", ""),
        os.environ.get("ARCHFLAGS", ""),
        plat_name_hint or "",
        os.environ.get("_PYTHON_HOST_PLATFORM", ""),
        sysconfig.get_platform(),
        platform.machine(),
    )
    for hint in hints:
        archs.update(extract_macos_architectures(str(hint)))

    if {"arm64", "x86_64"}.issubset(archs):
        return "universal2"
    if "arm64" in archs:
        return "arm64"
    if "x86_64" in archs:
        return "x86_64"
    return "arm64" if platform.machine().lower() in {"arm64", "aarch64"} else "x86_64"


def configure_macos_deployment_target(plat_name_hint: Optional[str] = None) -> Optional[str]:
    if sys.platform != "darwin":
        return None

    arch = infer_macos_architecture(plat_name_hint)
    target = os.environ.get("MACOSX_DEPLOYMENT_TARGET", "").strip()
    if not target:
        target = "11.0" if arch in {"arm64", "universal2"} else "10.15"
        os.environ["MACOSX_DEPLOYMENT_TARGET"] = target

    sysconfig.get_config_vars()["MACOSX_DEPLOYMENT_TARGET"] = target

    auto_host_platform = os.environ.get("DICOMSDL_AUTO_PYTHON_HOST_PLATFORM", "").strip()
    host_platform = os.environ.get("_PYTHON_HOST_PLATFORM", "").strip()
    if not host_platform or auto_host_platform in TRUTHY_ENV_VALUES:
        os.environ["_PYTHON_HOST_PLATFORM"] = f"macosx-{target}-{arch}"
        os.environ["DICOMSDL_AUTO_PYTHON_HOST_PLATFORM"] = "1"

    return target


configure_macos_deployment_target()


def refresh_temp_version_files(
    version_out: pathlib.Path = TEMP_VERSION_FILE,
    dicom_standard_out: pathlib.Path = TEMP_DICOM_STANDARD_VERSION_FILE,
) -> None:
    cmd = [
        sys.executable,
        str(VERSION_EXTRACTOR_SCRIPT),
        "--header",
        str(VERSION_HEADER_FILE),
        "--version-out",
        str(version_out),
        "--dicom-standard-out",
        str(dicom_standard_out),
    ]
    subprocess.check_call(cmd)


def load_version() -> str:
    refresh_temp_version_files()
    text = TEMP_VERSION_FILE.read_text(encoding="utf-8").strip()
    if not text:
        raise RuntimeError(f"Temporary version file {TEMP_VERSION_FILE} is empty")
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


def remove_windows_debug_sidecars(extdir: pathlib.Path) -> None:
    if sys.platform != "win32":
        return
    for pattern in ("*.ilk", "*.pdb"):
        for path in extdir.glob(pattern):
            if path.is_file():
                path.unlink()


class CMakeExtension(Extension):
    def __init__(self, name: str, sourcedir: str = "") -> None:
        super().__init__(name, sources=[])
        self.sourcedir = os.path.abspath(sourcedir) if sourcedir else str(ROOT_DIR)


class CMakeBuild(build_ext):
    def build_extension(self, ext: Extension) -> None:
        configure_macos_deployment_target(getattr(self, "plat_name", None))

        ext_full_path = pathlib.Path(self.get_ext_fullpath(ext.name)).resolve()
        extdir = ext_full_path.parent
        extdir.mkdir(parents=True, exist_ok=True)
        remove_stale_shared_artifacts(extdir)

        if isinstance(self.debug, str):
            self.debug = self.debug.strip().lower() in TRUTHY_ENV_VALUES

        # For wheel packaging flows, allow env-based forced Release even if
        # front-end tooling passes a debug build_ext flag unexpectedly.
        if parse_env_bool("FORCE_WHEEL_RELEASE", False):
            self.debug = False

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
            f"-DDICOMSDL_TEMP_VERSION_FILE={TEMP_VERSION_FILE}",
            f"-DDICOM_STANDARD_TEMP_VERSION_FILE={TEMP_DICOM_STANDARD_VERSION_FILE}",
            "-DDICOM_BUILD_PYTHON=ON",
            "-DDICOMSDL_PIXEL_CORE=ON",
            "-DDICOMSDL_PIXEL_RUNTIME=ON",
            "-DDICOMSDL_PIXEL_RLE_STATIC_PLUGIN=ON",
            f"-DCMAKE_BUILD_TYPE={cfg}",
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

        msvc_ltcg_mode = os.environ.get("DICOMSDL_MSVC_ENABLE_LTCG", "").strip()
        if msvc_ltcg_mode:
            cmake_args.append(f"-DDICOMSDL_MSVC_ENABLE_LTCG={msvc_ltcg_mode}")

        msvc_pgo_mode = os.environ.get("DICOMSDL_MSVC_PGO", "").strip()
        if msvc_pgo_mode:
            cmake_args.append(f"-DDICOMSDL_MSVC_PGO={msvc_pgo_mode}")
            msvc_pgo_dir = os.environ.get("DICOMSDL_MSVC_PGO_DIR", "").strip()
            if msvc_pgo_dir:
                cmake_args.append(f"-DDICOMSDL_MSVC_PGO_DIR={msvc_pgo_dir}")
            elif msvc_pgo_mode.upper() != "OFF":
                cmake_args.append(
                    f"-DDICOMSDL_MSVC_PGO_DIR={ROOT_DIR / 'build-pgo' / 'msvc'}"
                )

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

        if sys.platform == "darwin":
            deployment_target = os.environ.get("MACOSX_DEPLOYMENT_TARGET", "").strip()
            if deployment_target:
                cmake_args += [f"-DCMAKE_OSX_DEPLOYMENT_TARGET={deployment_target}"]

            arch_candidate = None
            plat_tokens = re.split(r"[-_]", plat_name_raw)
            if plat_tokens:
                maybe_arch = plat_tokens[-1]
                if maybe_arch in {"arm64", "x86_64"}:
                    arch_candidate = maybe_arch
            if arch_candidate:
                cmake_args += [f"-DCMAKE_OSX_ARCHITECTURES={arch_candidate}"]

        build_temp = pathlib.Path(self.build_temp) / build_suffix
        # Keep incremental CMake/Ninja state by default for faster local builds.
        clean_build = self.force or parse_env_bool("DICOMSDL_CLEAN_BUILD", False)
        if build_temp.exists() and clean_build:
            shutil.rmtree(build_temp)
        build_temp.mkdir(parents=True, exist_ok=True)

        subprocess.check_call(
            ["cmake", "-S", ext.sourcedir, "-B", str(build_temp)] + cmake_args # type: ignore
        )

        parallel_raw = (
            os.environ.get("DICOMSDL_BUILD_PARALLELISM")
            or os.environ.get("CMAKE_BUILD_PARALLEL_LEVEL")
        )
        if parallel_raw:
            try:
                parallel_jobs = int(parallel_raw)
            except ValueError as exc:
                raise RuntimeError(
                    "DICOMSDL_BUILD_PARALLELISM/CMAKE_BUILD_PARALLEL_LEVEL must be an integer"
                ) from exc
            if parallel_jobs < 1:
                raise RuntimeError(
                    "DICOMSDL_BUILD_PARALLELISM/CMAKE_BUILD_PARALLEL_LEVEL must be >= 1"
                )
        else:
            parallel_jobs = os.cpu_count() or 1

        subprocess.check_call(
            ["cmake", "--build", str(build_temp), "--parallel", str(parallel_jobs)] + build_args
        )

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
        remove_windows_debug_sidecars(extdir)


cmdclass = {"build_ext": CMakeBuild}

if bdist_wheel is not None:
    class DicomSdlBdistWheel(bdist_wheel):
        def finalize_options(self) -> None:
            configure_macos_deployment_target(getattr(self, "plat_name", None))
            super().finalize_options()
            configure_macos_deployment_target(getattr(self, "plat_name", None))

    cmdclass["bdist_wheel"] = DicomSdlBdistWheel


setup(
    name="dicomsdl",
    version=PACKAGE_VERSION,
    author="Kim, Tae-Sung",
    description="A fast and light-weighted DICOM software development library",
    long_description=README_TEXT,
    long_description_content_type="text/markdown",
    license="MIT",
    license_files=["LICENSE"],
    url=PROJECT_URL,
    project_urls=PROJECT_URLS,
    python_requires=">=3.9",
    extras_require={
        "numpy": ["numpy"],
        "pil": ["numpy", "Pillow"],
    },
    classifiers=[
        "Development Status :: 4 - Beta",
        "Intended Audience :: Developers",
        "Intended Audience :: Healthcare Industry",
        "Intended Audience :: Science/Research",
        "License :: OSI Approved :: MIT License",
        "Operating System :: MacOS :: MacOS X",
        "Operating System :: Microsoft :: Windows",
        "Operating System :: POSIX :: Linux",
        "Programming Language :: Python :: 3",
        "Programming Language :: Python :: 3.9",
        "Programming Language :: Python :: 3.10",
        "Programming Language :: Python :: 3.11",
        "Programming Language :: Python :: 3.12",
        "Programming Language :: Python :: 3.13",
        "Programming Language :: Python :: 3.14",
        "Programming Language :: C++",
        "Topic :: Scientific/Engineering",
        "Topic :: Scientific/Engineering :: Medical Science Apps.",
    ],
    packages=["dicomsdl"],
    package_dir={"dicomsdl": "bindings/python/dicomsdl"},
    package_data={"dicomsdl": ["py.typed", "*.pyi", "*.dll", "*.so", "*.dylib"]},
    ext_modules=[CMakeExtension("dicomsdl._dicomsdl")],
    cmdclass=cmdclass,
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
