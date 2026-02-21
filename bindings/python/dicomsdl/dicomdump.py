from __future__ import annotations

import argparse
import glob
import sys

import dicomsdl as dicom


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="dicomdump",
        description="Print a human-readable DICOM dump.",
    )
    parser.add_argument(
        "paths",
        nargs="+",
        help="DICOM file paths (supports wildcard patterns)",
    )
    parser.add_argument(
        "--max-print-chars",
        type=int,
        default=80,
        help="Max printable width per line before VALUE truncation (default: 80)",
    )
    parser.add_argument(
        "--no-offset",
        action="store_true",
        help="Hide OFFSET column in dump output",
    )
    parser.add_argument(
        "--with-filename",
        action="store_true",
        help="Prefix each output line with 'filename:' (default when multiple inputs)",
    )
    return parser


def expand_paths(inputs: list[str]) -> list[str]:
    expanded: list[str] = []
    for value in inputs:
        if glob.has_magic(value):
            matches = sorted(glob.glob(value))
            if matches:
                expanded.extend(matches)
                continue
        expanded.append(value)
    return expanded


def write_dump_text(text: str, path: str, with_filename: bool) -> None:
    if with_filename:
        for line in text.splitlines(keepends=True):
            if line.endswith("\n"):
                sys.stdout.write(f"{path}:{line}")
            else:
                sys.stdout.write(f"{path}:{line}\n")
        return

    sys.stdout.write(text)
    if text and not text.endswith("\n"):
        sys.stdout.write("\n")


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)

    paths = expand_paths(args.paths)
    with_filename = args.with_filename or len(paths) > 1
    exit_code = 0

    for path in paths:
        try:
            df = dicom.read_file(path)
            text = df.dump(
                max_print_chars=max(0, args.max_print_chars),
                include_offset=not args.no_offset,
            )
            write_dump_text(text, path, with_filename)
        except Exception as exc:
            print(f"dicomdump: {path}: {exc}", file=sys.stderr)
            exit_code = 1

    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
