#!/usr/bin/env python3

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path
import xml.etree.ElementTree as ET

from _docbook_common import (
    DB_NS,
    XML_ID_ATTR,
    build_parent_map,
    ensure_xml,
    find_table_by_id,
    first_link_target,
    nearest_ancestor,
    text_content,
    title_text,
)


PART03_URL = "http://dicom.nema.org/medical/dicom/current/source/docbook/part03/part03.xml"
PART04_URL = "http://dicom.nema.org/medical/dicom/current/source/docbook/part04/part04.xml"
OUTPUT_COLUMNS = (
    "uid_value",
    "sop_class_name",
    "sop_class_keyword",
    "iod_xml_id",
    "iod_title",
    "part04_section_id",
    "retired",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--part04", default=None, help="Path to PS3.4 DocBook XML")
    parser.add_argument("--part03", default=None, help="Path to PS3.3 DocBook XML")
    parser.add_argument(
        "--uid-registry",
        type=Path,
        default=Path("misc/dictionary/_uid_registry.tsv"),
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("misc/dictionary/_sopclass_iod_map.tsv"),
    )
    return parser.parse_args()


def load_uid_keywords(path: Path) -> dict[str, str]:
    keywords: dict[str, str] = {}
    if not path.exists():
        return keywords
    with path.open("r", encoding="utf-8", newline="") as fh:
        reader = csv.reader(fh, delimiter="\t")
        for cols in reader:
            if not cols or cols[0].strip().lower() == "uid_value":
                continue
            if len(cols) < 3:
                continue
            uid_value = cols[0].strip()
            keyword = cols[2].strip()
            if uid_value:
                keywords[uid_value] = keyword
    return keywords


def load_part03_titles(root: ET.Element) -> dict[str, str]:
    titles: dict[str, str] = {}
    for section in root.findall(".//db:section", DB_NS):
        section_id = section.get(XML_ID_ATTR, "").strip()
        if section_id:
            titles[section_id] = title_text(section)
    return titles


def build_rows(
    part04_root: ET.Element,
    part03_titles: dict[str, str],
    uid_keywords: dict[str, str],
) -> list[list[str]]:
    table = find_table_by_id(part04_root, "table_B.5-1")
    if table is None:
        raise RuntimeError("Could not find PS3.4 table_B.5-1 (Standard SOP Classes)")

    parent_map = build_parent_map(part04_root)
    owner_section = nearest_ancestor(table, parent_map, "section")
    owner_section_id = owner_section.get(XML_ID_ATTR, "").strip() if owner_section is not None else ""

    tbody = table.find("db:tbody", DB_NS)
    if tbody is None:
        return []

    rows: list[list[str]] = []
    for tr in tbody.findall("db:tr", DB_NS):
        cells = tr.findall("db:td", DB_NS)
        if len(cells) < 3:
            continue
        sop_class_name = text_content(cells[0])
        uid_value = text_content(cells[1])
        iod_xml_id = first_link_target(cells[2])
        if not sop_class_name or not uid_value:
            continue
        specialization_text = text_content(cells[3]) if len(cells) > 3 else ""
        retired = "Y" if "retired" in sop_class_name.lower() or "retired" in specialization_text.lower() else "N"
        rows.append(
            [
                uid_value,
                sop_class_name,
                uid_keywords.get(uid_value, ""),
                iod_xml_id,
                part03_titles.get(iod_xml_id, ""),
                owner_section_id,
                retired,
            ]
        )
    return rows


def write_rows(rows: list[list[str]], output: Path) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", encoding="utf-8", newline="") as fh:
        writer = csv.writer(fh, delimiter="\t", lineterminator="\n")
        writer.writerow(OUTPUT_COLUMNS)
        writer.writerows(rows)


def main() -> int:
    args = parse_args()
    script_dir = Path(__file__).resolve().parent
    part04_path = ensure_xml(script_dir, args.part04, "part04.xml", PART04_URL)
    part03_path = ensure_xml(script_dir, args.part03, "part03.xml", PART03_URL)

    print(f"Parsing SOP Class to IOD map from {part04_path} ...", file=sys.stderr)
    part04_root = ET.parse(part04_path).getroot()
    part03_root = ET.parse(part03_path).getroot()
    uid_keywords = load_uid_keywords(args.uid_registry)
    part03_titles = load_part03_titles(part03_root)
    rows = build_rows(part04_root, part03_titles, uid_keywords)
    print(f"Parsed {len(rows)} SOP Class rows", file=sys.stderr)
    write_rows(rows, args.output)
    print(f"Wrote {args.output}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
