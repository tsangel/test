#!/usr/bin/env python3

"""Generate include/dataelement_registry.hpp from _dataelement_registry.tsv."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path


HEADER_TEMPLATE = r"""
// Auto-generated from {source}
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace dicom {{

struct DataElementEntry {{
    std::uint32_t tag_value;
    std::uint16_t vr_value;
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
    with source.open("r", encoding="utf-8", newline="") as fh:
        reader = csv.reader(fh, delimiter="\t")
        for line_no, cols in enumerate(reader, 1):
            if not cols or all(not col.strip() for col in cols):
                continue
            if cols[0].strip().lower() == "tag":
                # Header row
                continue
            if len(cols) != 6:
                raise ValueError(
                    f"expected 6 columns, got {len(cols)} (line {line_no})"
                )
            rows.append(cols)
    rows.sort(key=lambda row: tag_sort_key(row[0]))
    return rows


def escape(value: str) -> str:
    return value.replace('"', r"\"")


VR_MAP = {
    "AE": 1,
    "AS": 2,
    "AT": 3,
    "CS": 4,
    "DA": 5,
    "DS": 6,
    "DT": 7,
    "FD": 8,
    "FL": 9,
    "IS": 10,
    "LO": 11,
    "LT": 12,
    "OB": 13,
    "OD": 14,
    "OF": 15,
    "OV": 16,
    "OL": 17,
    "OW": 18,
    "PN": 19,
    "SH": 20,
    "SL": 21,
    "SQ": 22,
    "SS": 23,
    "ST": 24,
    "SV": 25,
    "TM": 26,
    "UC": 27,
    "UI": 28,
    "UL": 29,
    "UN": 30,
    "UR": 31,
    "US": 32,
    "UT": 33,
    "UV": 34,
}


def parse_tag_value(tag: str) -> int | None:
    cleaned = tag.replace("(", "").replace(")", "").replace(",", "").replace(" ", "")
    if len(cleaned) != 8:
        return None
    cleaned = "".join("0" if ch in "xX" else ch for ch in cleaned)
    try:
        return int(cleaned, 16)
    except ValueError:
        return None


def parse_vr_value(vr: str) -> int:
    code = vr.strip().upper()
    if len(code) >= 2:
        code = code[:2]
        return VR_MAP.get(code, 0)
    return 0


def write_header(rows: list[list[str]], dest: Path, source_path: Path) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    with dest.open("w", encoding="utf-8") as fh:
        fh.write(
            HEADER_TEMPLATE.format(
                source=source_path.as_posix(), count=len(rows)
            )
        )
        for idx, row in enumerate(rows):
            raw_tag, raw_name, raw_keyword, raw_vr, raw_vm, raw_retired = row
            tag_value = parse_tag_value(raw_tag)
            vr_value = parse_vr_value(raw_vr)
            tag_literal = (
                f"0x{tag_value:08X}u" if tag_value is not None else "0u"
            )
            vr_literal = f"{vr_value}u"
            tag, name, keyword, vr, vm, retired = (
                escape(raw_tag),
                escape(raw_name),
                escape(raw_keyword),
                escape(raw_vr),
                escape(raw_vm),
                escape(raw_retired),
            )
            fh.write(
                f'    /* {idx:4d} */ DataElementEntry{{{tag_literal}, {vr_literal}, "{tag}", "{name}", "{keyword}", "{vr}", "{vm}", "{retired}"}},\n'
            )
        fh.write(FOOTER)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--source",
        type=Path,
        default=Path("misc/dictionary/_dataelement_registry.tsv"),
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
