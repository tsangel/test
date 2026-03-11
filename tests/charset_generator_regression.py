#!/usr/bin/env python3
from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=pathlib.Path, required=True)
    parser.add_argument("--python", required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    root = args.root.resolve()
    python_exe = args.python

    generated_files = [
        root / "src" / "charset" / "generated" / "sbcs_to_unicode_selected.hpp",
        root / "src" / "charset" / "generated" / "gb18030_tables.hpp",
        root / "src" / "charset" / "generated" / "ksx1001_tables.hpp",
        root / "src" / "charset" / "generated" / "jisx0208_tables.hpp",
        root / "src" / "charset" / "generated" / "jisx0212_tables.hpp",
    ]
    original_bytes = {path: path.read_bytes() for path in generated_files}

    generator_scripts = [
        root / "misc" / "charset" / "generate_selected_sbcs_tables.py",
        root / "misc" / "charset" / "generate_gb18030_tables.py",
        root / "misc" / "charset" / "generate_ksx1001_tables.py",
        root / "misc" / "charset" / "generate_jis_tables.py",
    ]

    try:
        for script in generator_scripts:
            completed = subprocess.run(
                [python_exe, str(script)],
                cwd=root,
                capture_output=True,
                text=True,
                check=False,
            )
            if completed.returncode != 0:
                sys.stderr.write(
                    f"generator failed: {script.name}\n"
                    f"stdout:\n{completed.stdout}\n"
                    f"stderr:\n{completed.stderr}\n"
                )
                return 1

        for path in generated_files:
            current = path.read_bytes()
            if current != original_bytes[path]:
                sys.stderr.write(
                    "charset generator regression mismatch: "
                    f"{path.relative_to(root)} changed after regeneration\n"
                )
                return 2
    finally:
        for path, content in original_bytes.items():
            if path.read_bytes() != content:
                path.write_bytes(content)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
