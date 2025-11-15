#!/usr/bin/env python3
"""Generate include/specific_character_set_registry.hpp from _specific_character_sets.tsv."""

from __future__ import annotations

import argparse
import csv
import re
from dataclasses import dataclass
from pathlib import Path


@dataclass
class CharacterSetRow:
    table_id: str
    description: str
    defined_term: str
    standard: str
    esc_sequence: str
    iso_registration: str
    number_of_characters: str
    code_element: str
    character_set: str


HEADER = """// Auto-generated from {source}
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace dicom {{

enum class SpecificCharacterSet : std::uint16_t {{
    Unknown = 0,
{enum_entries}
}};

struct SpecificCharacterSetInfo {{
    SpecificCharacterSet value;
    std::string_view defined_term;
    std::string_view description;
}};

inline constexpr std::array<SpecificCharacterSetInfo, {count}> kSpecificCharacterSetInfo = {{
{info_entries}
}};

namespace detail {{

constexpr bool sv_equal(std::string_view lhs, std::string_view rhs) {{
    if (lhs.size() != rhs.size()) {{
        return false;
    }}
    for (std::size_t i = 0; i < lhs.size(); ++i) {{
        if (lhs[i] != rhs[i]) {{
            return false;
        }}
    }}
    return true;
}}

}} // namespace detail

inline constexpr SpecificCharacterSet specific_character_set_from_term(std::string_view term) {{
    for (const auto& info : kSpecificCharacterSetInfo) {{
        if (detail::sv_equal(info.defined_term, term)) {{
            return info.value;
        }}
    }}
    return SpecificCharacterSet::Unknown;
}}

inline constexpr const SpecificCharacterSetInfo* specific_character_set_info(SpecificCharacterSet set) {{
    for (const auto& info : kSpecificCharacterSetInfo) {{
        if (info.value == set) {{
            return &info;
        }}
    }}
    return nullptr;
}}

}} // namespace dicom
"""


def load_rows(path: Path) -> list[CharacterSetRow]:
    rows: list[CharacterSetRow] = []
    with path.open("r", encoding="utf-8", newline="") as fh:
        reader = csv.DictReader(fh, delimiter="\t")
        for raw in reader:
            defined_term = raw["defined_term"].strip()
            if not defined_term:
                continue
            rows.append(
                CharacterSetRow(
                    table_id=raw["table_id"].strip(),
                    description=raw["character_set_description"].strip(),
                    defined_term=defined_term,
                    standard=raw["standard_for_code_extension"].strip(),
                    esc_sequence=raw["esc_sequence"].strip(),
                    iso_registration=raw["iso_registration_number"].strip(),
                    number_of_characters=raw["number_of_characters"].strip(),
                    code_element=raw["code_element"].strip(),
                    character_set=raw["character_set"].strip(),
                )
            )
    return rows


def dedupe_rows(rows: list[CharacterSetRow]) -> list[CharacterSetRow]:
    seen: set[str] = set()
    deduped: list[CharacterSetRow] = []
    for row in rows:
        if row.defined_term in seen:
            continue
        seen.add(row.defined_term)
        deduped.append(row)
    return deduped


def ensure_iso_ir_6(rows: list[CharacterSetRow]) -> list[CharacterSetRow]:
    if any(row.defined_term == "ISO_IR 6" for row in rows):
        return rows
    iso_row = CharacterSetRow(
        table_id="table_C.12-2",
        description="Default repertoire",
        defined_term="ISO_IR 6",
        standard="",
        esc_sequence="",
        iso_registration="ISO-IR 6",
        number_of_characters="94",
        code_element="G0",
        character_set="",
    )
    insert_index = 1 if rows and rows[0].defined_term == "none" else 0
    rows.insert(insert_index, iso_row)
    return rows


def make_enum_name(term: str) -> str:
    token = re.sub(r"[^0-9A-Za-z]+", "_", term.strip().upper())
    token = re.sub(r"__+", "_", token).strip("_")
    if not token:
        token = "UNKNOWN"
    if token[0].isdigit():
        token = f"SCS_{token}"
    return token


def esc(value: str) -> str:
    return value.replace("\\", r"\\").replace('"', r"\"")


def format_entries(rows: list[CharacterSetRow]) -> tuple[str, str]:
    enum_lines = []
    info_lines = []
    for idx, row in enumerate(rows, start=1):
        enum_lines.append(f"    {make_enum_name(row.defined_term)} = {idx},")
        info_lines.append(
            "    {" +
            f"SpecificCharacterSet::{make_enum_name(row.defined_term)}, "
            f"\"{esc(row.defined_term)}\", "
            f"\"{esc(row.description)}\"" +
            "},"
        )
    return "\n".join(enum_lines), "\n".join(info_lines)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path("misc/dictionary/_specific_character_sets.tsv"))
    parser.add_argument("--output", type=Path, default=Path("include/specific_character_set_registry.hpp"))
    args = parser.parse_args()

    rows = ensure_iso_ir_6(dedupe_rows(load_rows(args.source)))
    enum_entries, info_entries = format_entries(rows)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        HEADER.format(source=args.source.as_posix(), enum_entries=enum_entries, info_entries=info_entries, count=len(rows)),
        encoding="utf-8",
    )


if __name__ == "__main__":
    main()
