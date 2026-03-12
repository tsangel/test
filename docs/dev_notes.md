# Developer notes

## Build & test
- C++: `cmake -S . -B build -DDICOM_BUILD_EXAMPLES=ON -DCMAKE_BUILD_TYPE=Release && cmake --build build && ctest --test-dir build`
- Python: build `_dicomsdl`, `py -3.14 -m pip install -r tests/python/requirements.txt`, then `py -3.14 -m pytest tests/python -q`
- Submodules: `git submodule update --init --recursive` (includes nanobind)

## Dictionary/registry generation
- Sources: `misc/dictionary/part06.xml`, `part03.xml`, `_dicom_version.txt`, `include/dicom_const.h`, etc.
- Scripts: `misc/dictionary/extract_part06_tables.py` and `generate_*` produce TSV → headers (`include/dataelement_registry.hpp`, `include/dataelement_lookup_tables.hpp`, `include/uid_registry.hpp`, `include/specific_character_set_registry.hpp`).
- Version extraction: `CMakeLists.txt` parses `include/dicom_const.h` and writes temporary VERSION cache files under the build directory.

## Naming/style
- C++ public API uses snake_case; private members use trailing underscore (e.g., `path_`).
- Python: recommend `import dicomsdl as dicom` alias.
- Errors: invalid keyword/tag strings throw exceptions (`std::invalid_argument` or `ValueError`).

## Docs build pipeline
- RTD runs `doxygen Doxyfile` to produce XML, then Sphinx+Breathe consumes it for the C++ API.
- Content uses Sphinx + MyST Markdown with the `furo` theme.
- PDF/ePub outputs are enabled.
