# Developer notes

```{note}
本页正文目前仍为英文原文。需要时请以英文版为准。
```

## Build & test
- C++: `cmake -S . -B build -DDICOM_BUILD_EXAMPLES=ON -DCMAKE_BUILD_TYPE=Release && cmake --build build && ctest --test-dir build`
- Python: build `_dicomsdl`, `py -3.14 -m pip install -r tests/python/requirements.txt`, then `py -3.14 -m pytest tests/python -q`
- Third-party dependencies are fetched by CMake `FetchContent` during configure

## Dictionary/registry generation
- Sources: `misc/dictionary/part06.xml`, `part03.xml`, `_dicom_version.txt`, `include/dicom_const.h`, etc.
- Scripts: `misc/dictionary/extract_part06_tables.py` and `generate_*` produce TSV → headers (`include/dataelement_registry.hpp`, `include/dataelement_lookup_tables.hpp`, `include/uid_registry.hpp`, `include/specific_character_set_registry.hpp`).
- Version extraction: `CMakeLists.txt` parses `include/dicom_const.h` and writes temporary VERSION cache files under the build directory.
- Docs version: `docs/conf.py` also reads `DICOMSDL_VERSION` directly from `include/dicom_const.h`.

## Naming/style
- C++ public API uses snake_case; private members use trailing underscore (e.g., `path_`).
- Python: recommend `import dicomsdl as dicom` alias.
- Errors: invalid keyword/tag strings throw exceptions (`std::invalid_argument` or `ValueError`).

## Docs build pipeline
- RTD runs `doxygen Doxyfile` to produce XML, then Sphinx+Breathe consumes it for the C++ API.
- Content uses Sphinx + MyST Markdown with the `furo` theme.
- English source pages live under `docs/en`, and localized source trees mirror the same relative paths under `docs/ko`, `docs/ja`, and `docs/zh-cn`.
- `./build-docs.sh check` verifies that localized trees match the English path set.
- `./build-docs.sh html` builds the current language (`DICOMSDL_DOC_LANGUAGE`, default `en`) from `docs/<lang>`.
- `./build-docs.sh html-all` builds `en`, `ko`, `ja`, and `zh-cn`.
- The public documentation structure is split into `Guide`, `Reference`, and `Developer`; see [Translation Workflow](translation_workflow.md) for the localization process.
- PDF/ePub outputs are enabled.
