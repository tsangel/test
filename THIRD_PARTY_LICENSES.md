# Third-Party Licenses

This file summarizes the third-party libraries that DicomSDL fetches or builds
through CMake and the license terms that apply to them.

This summary is for maintenance and packaging convenience only. It is not legal
advice. When redistributing binaries or source, check the upstream license text
for each dependency and any feature-specific subdependency it brings in.

Source of truth for the dependency list:

- [cmake/DicomsdlDependencies.cmake](cmake/DicomsdlDependencies.cmake)
- [cmake/DicomsdlPython.cmake](cmake/DicomsdlPython.cmake)

## DicomSDL Itself

- DicomSDL
  - License: MIT
  - Local file: [LICENSE](LICENSE)

## Direct Dependencies

These are the direct third-party projects referenced by DicomSDL's CMake build.

| Library | Version/tag in CMake | Used for | Default status | License summary |
|---|---|---|---|---|
| `fmt` | `12.1.0` | formatting | enabled | MIT |
| `yyjson` | `0.12.0` | DICOM JSON parsing | enabled | MIT |
| `openjpeg` | `v2.5.4` | JPEG 2000 decode/encode support | enabled | BSD 2-Clause |
| `libjpeg-turbo` | `3.1.3` | JPEG/TurboJPEG support | enabled | IJG License + Modified BSD 3-Clause; some SIMD sources also carry zlib-style notices |
| `libdeflate` | `v1.25` | deflate compression helpers | enabled | MIT |
| `CharLS` | `2.4.3` | JPEG-LS support | enabled | BSD 3-Clause |
| `OpenJPH` | `0.26.3` | HTJ2K support | enabled by default via `DICOMSDL_ENABLE_OPENJPH=ON` | BSD 2-Clause |
| `nanobind` | `v2.12.0` | Python bindings | enabled when `DICOM_BUILD_PYTHON=ON` | BSD 3-Clause |
| `libjxl` | `v0.11.2` | JPEG XL support | disabled by default via `DICOMSDL_ENABLE_JPEGXL=OFF` | BSD 3-Clause |

## Upstream License Files Checked

The license names above were verified against the upstream license files from
the local FetchContent checkouts used during development.

- `fmt`
  - Upstream license file: `LICENSE`
  - License family: MIT
- `yyjson`
  - Upstream license file: `LICENSE`
  - License family: MIT
- `openjpeg`
  - Upstream license file: `LICENSE`
  - License family: BSD 2-Clause
- `libjpeg-turbo`
  - Upstream license file: `LICENSE.md`
  - License family: mixed
  - Notes:
    - libjpeg API side: IJG License
    - TurboJPEG/build system side: Modified BSD 3-Clause
    - SIMD sources: zlib-style notices are present in some files
- `libdeflate`
  - Upstream license file: `COPYING`
  - License family: MIT
- `CharLS`
  - Upstream license file: `LICENSE.md`
  - License family: BSD 3-Clause
- `OpenJPH`
  - Upstream license file: `LICENSE`
  - License family: BSD 2-Clause
- `nanobind`
  - Upstream license file: `LICENSE`
  - License family: BSD 3-Clause
- `libjxl`
  - Upstream license file: `LICENSE.jpeg-xl` in generated build notices
  - License family: BSD 3-Clause

## Notable Bundled or Transitive License Notes

Some optional dependencies bring their own bundled third-party code.

- `nanobind`
  - Fetches `ext/robin_map`
  - `robin_map` license: MIT
- `libjxl`
  - Common bundled notices seen in local builds:
    - Brotli: MIT
    - Highway: Apache 2.0
    - skcms: BSD 3-Clause
  - Exact bundled set depends on the `libjxl` version and build options
- `openjpeg`
  - Upstream source tree includes additional third-party material under
    `thirdparty/`, but DicomSDL links against the generated `openjp2` library
    rather than redistributing every tool/example from the source tree

## Packaging Notes

- The exact set of bundled third-party libraries depends on enabled features.
- The default C++ library build enables `OpenJPH` and disables `libjxl`.
- Python wheels or local Python extension builds also include `nanobind`.
- If you distribute binaries, include the full upstream license texts required
  by the enabled dependency set rather than relying only on this summary.
