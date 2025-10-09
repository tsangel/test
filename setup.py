import os
import os
import pathlib
import shutil
import subprocess
import sys
from setuptools import Extension, setup
from setuptools.command.build_ext import build_ext

ROOT_DIR = pathlib.Path(__file__).resolve().parent


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

        build_temp = pathlib.Path(self.build_temp) / ext.name
        build_temp.mkdir(parents=True, exist_ok=True)

        subprocess.check_call(
            ["cmake", "-S", ext.sourcedir, "-B", str(build_temp)] + cmake_args
        )
        subprocess.check_call(["cmake", "--build", str(build_temp)] + build_args)

        built_artifact = None
        module_basename = ext.name.split(".")[-1]
        search_patterns = [
            f"{module_basename}*.so",
            f"{module_basename}*.pyd",
            f"lib{module_basename}*.dylib",
        ]

        search_roots = [
            build_temp,
            build_temp.parent,
            ROOT_DIR / "build",
        ]

        for root in search_roots:
            for pattern in search_patterns:
                matches = sorted(root.rglob(pattern))
                if matches:
                    built_artifact = matches[0]
                    break
            if built_artifact:
                break

        if built_artifact is None:
            raise RuntimeError(f"Cannot find built extension for {module_basename} in {build_temp}")

        shutil.copyfile(built_artifact, ext_full_path)


setup(
    name="dicomsdl",
    version="0.1.0",
    author="DicomProject Authors",
    description="Python bindings for dicomsdl",
    python_requires=">=3.8",
    packages=["dicomsdl"],
    package_dir={"dicomsdl": "bindings/python/dicomsdl"},
    ext_modules=[CMakeExtension("dicomsdl._dicomsdl")],
    cmdclass={"build_ext": CMakeBuild},
    zip_safe=False,
)
