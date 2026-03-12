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
        root / "include" / "dataelement_registry.hpp",
        root / "include" / "dataelement_lookup_tables.hpp",
        root / "include" / "uid_registry.hpp",
        root / "include" / "uid_lookup_tables.hpp",
        root / "include" / "specific_character_set_registry.hpp",
    ]
    original_bytes = {path: path.read_bytes() for path in generated_files}

    generator_commands = [
        [
            python_exe,
            "misc/dictionary/generate_dataelement_registry.py",
            "--source",
            "misc/dictionary/_dataelement_registry.tsv",
            "--output",
            "include/dataelement_registry.hpp",
        ],
        [
            python_exe,
            "misc/dictionary/generate_lookup_tables.py",
            "--registry",
            "misc/dictionary/_dataelement_registry.tsv",
            "--output",
            "include/dataelement_lookup_tables.hpp",
        ],
        [
            python_exe,
            "misc/dictionary/generate_uid_registry.py",
            "--source",
            "misc/dictionary/_uid_registry.tsv",
            "--output",
            "include/uid_registry.hpp",
        ],
        [
            python_exe,
            "misc/dictionary/generate_uid_lookup_tables.py",
            "--source",
            "misc/dictionary/_uid_registry.tsv",
            "--output",
            "include/uid_lookup_tables.hpp",
        ],
        [
            python_exe,
            "misc/dictionary/generate_specific_character_set_registry.py",
            "--source",
            "misc/dictionary/_specific_character_sets.tsv",
            "--output",
            "include/specific_character_set_registry.hpp",
        ],
    ]

    try:
        for command in generator_commands:
            completed = subprocess.run(
                command,
                cwd=root,
                capture_output=True,
                text=True,
                check=False,
            )
            if completed.returncode != 0:
                sys.stderr.write(
                    f"generator failed: {pathlib.Path(command[1]).name}\n"
                    f"stdout:\n{completed.stdout}\n"
                    f"stderr:\n{completed.stderr}\n"
                )
                return 1

        for path in generated_files:
            current = path.read_bytes()
            if current != original_bytes[path]:
                sys.stderr.write(
                    "dictionary generator regression mismatch: "
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
