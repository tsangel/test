# Translation Workflow

English documentation is the source of truth for `dicomsdl`.
Every public English page should have matching document files for Korean (`ko`), Japanese (`ja`), and Simplified Chinese (`zh-cn`).

## Scope

- English source pages live under `docs/en/`.
- Localized source pages live under `docs/ko/`, `docs/ja/`, and `docs/zh-cn/`.
- Each localized document keeps the same relative path as its English counterpart.
- Repository entry points use dedicated Markdown counterparts:
  - `README.md`
  - `README.ko.md`
  - `README.ja.md`
  - `README.zh-CN.md`

## Information architecture

- `Guide`: onboarding, installation, and common usage paths
- `Reference`: exact API behavior, constraints, and semantics
- `Developer`: contributor workflow, benchmarks, and documentation operations

The same page set should exist across English, Korean, Japanese, and Simplified Chinese builds. If a translation is not ready yet, keep the file in place and mark it as draft instead of deleting it.

## Build commands

Install the docs toolchain first:

```bash
python -m pip install -r docs/requirements.txt
```

Then use the wrapper script:

```bash
# English HTML (default)
./build_docs.sh html

# Verify that ko / ja / zh-cn mirror docs/en paths
./build_docs.sh check

# Build a single localized site when translation work starts
DICOMSDL_DOC_LANGUAGE=ko ./build_docs.sh html

# Build English only into docs/_build/html-multilang/en
./build_docs.sh html-all

# Also build ko / ja / zh-cn placeholders or active translations
DICOMSDL_BUILD_TRANSLATIONS=1 ./build_docs.sh html-all
```

While English is still the active writing language, the localized trees may contain placeholder files only.
The default `html-all` build stays English-only so Sphinx does not emit `toc.not_included` warnings for empty localized pages.

## Editing workflow

1. Update the English page first.
2. Apply the same path change under `docs/ko`, `docs/ja`, and `docs/zh-cn`.
3. Translate the whole target document, not isolated message fragments.
4. Run `./build_docs.sh check` to catch missing or extra files.
5. Build English and review it first. Build a target language only when localized content exists.
6. Keep localized README files aligned with the English README when repo-level onboarding changes.

## Translation rules

- Keep code, CLI flags, environment variables, DICOM tags, UIDs, and VR names in English.
- Translate explanatory prose, headings, and user-facing guidance.
- Preserve MyST/Sphinx markup, cross-references, and inline code spans.
- Prefer document-level rewrites over sentence-by-sentence literal translation for CJK languages.
- Keep each localized file at the same relative path as the English source so links and toctrees stay consistent.
