# dicomsdl Documentation

Lightweight DICOM parser/wrapper with a C++ core and a pybind11-based Python module. The docs are written with Python users first, while still covering the C++ API.

## What you'll find here
- Installation and build (Python wheel, CMake-based C++)
- Core examples: keyword↔tag/VR lookup, reading/iterating/adding `DataSet`
- Unified API reference: Python overview plus C++ types via Doxygen + Breathe
- Developer guide: dictionary generation scripts, build/test workflow

```{toctree}
:maxdepth: 2
:caption: Guide

quickstart
python_api
python_reference
cpp_api
dev_notes
LOGGING
python_dataset_access_benchmarks
tag_path_lookup
```

## Community & contributions
- Issues and PRs live in the GitHub repository.
- Code comments and public docs should be in English.
