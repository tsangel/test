#!/usr/bin/env python3

from __future__ import annotations

import sys
from pathlib import Path


LANGUAGES = ("ko", "ja", "zh-cn")
INCLUDE_SUFFIXES = {".md", ".rst"}


def collect_docs(root: Path) -> set[Path]:
    return {
        path.relative_to(root)
        for path in root.rglob("*")
        if path.is_file() and path.suffix in INCLUDE_SUFFIXES
    }


def main() -> int:
    docs_root = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path("docs").resolve()
    source_root = docs_root / "en"
    expected = collect_docs(source_root)
    if not expected:
        print(f"error: no source docs found under {source_root}", file=sys.stderr)
        return 1

    failed = False
    for lang in LANGUAGES:
        lang_root = docs_root / lang
        actual = collect_docs(lang_root)
        missing = sorted(expected - actual)
        extra = sorted(actual - expected)

        if not missing and not extra:
            print(f"[ok] {lang}: {len(expected)} files")
            continue

        failed = True
        print(f"[error] {lang} is out of sync with docs/en")
        for relpath in missing:
            print(f"  missing: {relpath}")
        for relpath in extra:
            print(f"  extra:   {relpath}")

    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
