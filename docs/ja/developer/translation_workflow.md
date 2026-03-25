# Translation Workflow

```{note}
このページ本文はまだ英語の原文です。必要に応じて英語版を基準に参照してください。
```

English documentation is the source of truth for DicomSDL.
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
./build-docs.sh html

# Verify that ko / ja / zh-cn mirror docs/en paths
./build-docs.sh check

# Build a single localized site when translation work starts
DICOMSDL_DOC_LANGUAGE=ko ./build-docs.sh html

# Build en / ko / ja / zh-cn into docs/_build/html-multilang/
./build-docs.sh html-all
```

`html-all` now builds all supported languages by default.

## Editing workflow

1. Update the English page first.
2. Apply the same path change under `docs/ko`, `docs/ja`, and `docs/zh-cn`.
3. Translate the whole target document, not isolated message fragments.
4. Run `./build-docs.sh check` to catch missing or extra files.
5. Build English and review it first. Build a target language only when localized content exists.
6. Keep localized README files aligned with the English README when repo-level onboarding changes.

## Translation rules

- Keep code, CLI flags, environment variables, DICOM tags, UIDs, and VR names in English.
- Translate explanatory prose, headings, and user-facing guidance.
- Preserve MyST/Sphinx markup, cross-references, and inline code spans.
- Prefer document-level rewrites over sentence-by-sentence literal translation for CJK languages.
- Keep each localized file at the same relative path as the English source so links and toctrees stay consistent.
