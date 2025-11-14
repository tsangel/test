import os
import pathlib
import re
import shutil
import subprocess
import sys
import sysconfig
from setuptools import Extension, setup
from setuptools.command.build_ext import build_ext

ROOT_DIR = pathlib.Path(__file__).resolve().parent
VERSION_FILE = ROOT_DIR / "VERSION"

def load_version() -> str:
    text = VERSION_FILE.read_text(encoding="utf-8").strip()
    if not text:
        raise RuntimeError(f"Version file {VERSION_FILE} is empty")
    return text

PACKAGE_VERSION = load_version()


class CMakeExtension(Extension):
    def __init__(self, name: str, sourcedir: str = "") -> None:
        super().__init__(name, sources=[])
        self.sourcedir = os.path.abspath(sourcedir) if sourcedir else str(ROOT_DIR)


class CMakeBuild(build_ext):
    def build_extension(self, ext: Extension) -> None:
        ext_full_path = pathlib.Path(self.get_ext_fullpath(ext.name)).resolve()
        extdir = ext_full_path.parent
        extdir.mkdir(parents=True, exist_ok=True)

        cfg = "Debug" if self.debug else "Release"

        python_executable = sys.executable
        python_tag = sysconfig.get_python_version().replace(".", "")
        plat_name_raw = getattr(self, "plat_name", sysconfig.get_platform())
        plat_name = plat_name_raw.replace("-", "_")
        build_suffix = f"{ext.name.replace('.', '_')}_{python_tag}_{plat_name}"

        cmake_args = [
            f"-DCMAKE_LIBRARY_OUTPUT_DIRECTORY={extdir}",
            f"-DPYTHON_EXECUTABLE={python_executable}",
            f"-DPython_EXECUTABLE={python_executable}",
            f"-DPython3_EXECUTABLE={python_executable}",
            "-DDICOM_BUILD_PYTHON=ON",
            "-DPYBIND11_FINDPYTHON=ON",
        ]

        build_args = ["--target", "_dicomsdl"]

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

        if not ext_full_path.exists():
            raise RuntimeError(f"Expected extension at {ext_full_path} not found")


setup(
    name="dicomsdl",
    version=PACKAGE_VERSION,
    author="DicomProject Authors",
    description="Python bindings for dicomsdl",
    python_requires=">=3.8",
    packages=["dicomsdl"],
    package_dir={"dicomsdl": "bindings/python/dicomsdl"},
    package_data={"dicomsdl": ["py.typed", "*.pyi"]},
    ext_modules=[CMakeExtension("dicomsdl._dicomsdl")],
    cmdclass={"build_ext": CMakeBuild},
    include_package_data=True,
    zip_safe=False,
)
