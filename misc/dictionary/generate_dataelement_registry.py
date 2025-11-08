#!/usr/bin/env python3

"""Generate include/dataelement_registry.hpp from _dataelement_registry.txt."""

from __future__ import annotations

import argparse
from pathlib import Path


HEADER_TEMPLATE = r"""
// Auto-generated from {source}
#pragma once

#include <array>
#include <string_view>

namespace dicom {{

struct DataElementEntry {{
    std::string_view tag;
    std::string_view name;
    std::string_view keyword;
    std::string_view vr;
    std::string_view vm;
    std::string_view retired;
}};

constexpr std::array<DataElementEntry, {count}> kDataElementRegistry = {{
"""

FOOTER = r"""};

} // namespace dicom
"""


def tag_sort_key(tag: str) -> int:
    tag = tag.strip()[1:-1]
    group, element = tag.replace("x", "0").replace("X", "0").split(",")
    return (int(group, 16) << 16) | int(element, 16)


def parse_rows(source: Path) -> list[list[str]]:
    rows: list[list[str]] = []
    for line in source.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        cols = line.split("\t")
        if len(cols) != 6:
            raise ValueError(f"expected 6 columns, got {len(cols)} in line: {line}")
        rows.append(cols)
    rows.sort(key=lambda row: tag_sort_key(row[0]))
    return rows


def escape(value: str) -> str:
    return value.replace('"', r"\"")


def write_header(rows: list[list[str]], dest: Path, source_path: Path) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    with dest.open("w", encoding="utf-8") as fh:
        fh.write(
            HEADER_TEMPLATE.format(
                source=source_path.as_posix(), count=len(rows)
            )
        )
        for idx, row in enumerate(rows):
            tag, name, keyword, vr, vm, retired = (escape(v) for v in row)
            fh.write(
                f'    /* {idx:4d} */ DataElementEntry{{"{tag}", "{name}", "{keyword}", "{vr}", "{vm}", "{retired}"}},\n'
            )
        fh.write(FOOTER)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--source",
        type=Path,
        default=Path("misc/dictionary/_dataelement_registry.txt"),
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("include/dataelement_registry.hpp"),
    )
    args = parser.parse_args()
    rows = parse_rows(args.source)
    write_header(rows, args.output, args.source)


if __name__ == "__main__":
    main()
