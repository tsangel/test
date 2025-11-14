#!/usr/bin/env python3

"""Extract Specific Character Set tables (C.12-2..C.12-5) from part03.xml."""

from __future__ import annotations

import csv
import sys
import urllib.error
import urllib.request
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterator, List, Sequence


DB_NS = {
    "db": "http://docbook.org/ns/docbook",
    "xml": "http://www.w3.org/XML/1998/namespace",
}
ZERO_WIDTH = dict.fromkeys(map(ord, "\u200b\u200c\u200d"), None)
PART03_URL = "http://dicom.nema.org/medical/dicom/current/source/docbook/part03/part03.xml"


@dataclass(frozen=True)
class TableSpec:
    xml_id: str
    label: str | None = None


TABLE_SPECS: Sequence[TableSpec] = (
    TableSpec("table_C.12-2", "C.12-2"),
    TableSpec("table_C.12-3", "C.12-3"),
    TableSpec("table_C.12-4", "C.12-4"),
    TableSpec("table_C.12-5", "C.12-5"),
)

HEADER_MAP: Dict[str, str] = {
    "Character Set Description": "character_set_description",
    "Defined Term": "defined_term",
    "Standard for Code Extension": "standard_for_code_extension",
    "ESC Sequence": "esc_sequence",
    "ISO Registration Number": "iso_registration_number",
    "Number of Characters": "number_of_characters",
    "Code Element": "code_element",
    "Character Set": "character_set",
}

OUTPUT_COLUMNS: Sequence[str] = (
    "table_id",
    "table_label",
    "character_set_description",
    "defined_term",
    "standard_for_code_extension",
    "esc_sequence",
    "iso_registration_number",
    "number_of_characters",
    "code_element",
    "character_set",
)


def _text_content(element: ET.Element) -> str:
    parts: List[str] = []
    for node in element.iter():
        if node.text:
            parts.append(node.text)
        if node.tail:
            parts.append(node.tail)
    text = "".join(parts)
    if not text:
        return ""
    text = text.translate(ZERO_WIDTH)
    text = text.replace("\u00a0", " ")
    return " ".join(text.split())


def _extract_headers(table: ET.Element) -> List[str]:
    thead = table.find("db:thead", DB_NS)
    if thead is None:
        raise RuntimeError("Table is missing <thead> section")
    rows = thead.findall("db:tr", DB_NS)
    if not rows:
        raise RuntimeError("Table header is empty")
    header_row = rows[-1]
    headers: List[str] = []
    for th in header_row.findall("db:th", DB_NS):
        text = _text_content(th)
        colspan = int(th.get("colspan", "1") or "1")
        for _ in range(colspan):
            headers.append(text)
    return headers


def _iter_body_rows(table: ET.Element, column_count: int) -> Iterator[List[str]]:
    tbody = table.find("db:tbody", DB_NS)
    if tbody is None:
        return
    pending: List[tuple[str, int] | None] = [None] * column_count
    for tr in tbody.findall("db:tr", DB_NS):
        row: List[str | None] = [None] * column_count
        occupied = [False] * column_count
        for idx, pending_cell in enumerate(pending):
            if pending_cell is None:
                continue
            text, rows_left = pending_cell
            row[idx] = text
            occupied[idx] = True
            rows_left -= 1
            pending[idx] = (text, rows_left) if rows_left > 0 else None
        col_idx = 0
        for td in tr.findall("db:td", DB_NS):
            while col_idx < column_count and occupied[col_idx]:
                col_idx += 1
            text = _text_content(td)
            colspan = int(td.get("colspan", "1") or "1")
            rowspan = int(td.get("rowspan", "1") or "1")
            for offset in range(colspan):
                idx = col_idx + offset
                if idx >= column_count:
                    break
                row[idx] = text
                occupied[idx] = True
                if rowspan > 1:
                    pending[idx] = (text, rowspan - 1)
            col_idx += colspan
        yield [cell if cell is not None else "" for cell in row]


def build_specific_character_set_rows(xml_path: Path) -> List[Dict[str, str]]:
    tree = ET.parse(xml_path)
    root = tree.getroot()
    rows: List[Dict[str, str]] = []
    for spec in TABLE_SPECS:
        table = root.find(f".//db:table[@xml:id='{spec.xml_id}']", DB_NS)
        if table is None:
            raise RuntimeError(f"Unable to locate table {spec.xml_id} in {xml_path}")
        headers = _extract_headers(table)
        column_count = len(headers)
        table_label = table.get("label") or spec.label or spec.xml_id
        for row_values in _iter_body_rows(table, column_count):
            if not any(value for value in row_values):
                continue
            record = {key: "" for key in OUTPUT_COLUMNS}
            record["table_id"] = spec.xml_id
            record["table_label"] = table_label
            for header, value in zip(headers, row_values):
                key = HEADER_MAP.get(header)
                if key:
                    record[key] = value
            rows.append(record)
    return rows


def _report_progress(prefix: str, transferred: int, total: int | None) -> None:
    if total and total > 0:
        percent = transferred / total * 100
        print(f"{prefix}: {percent:5.1f}% ({transferred}/{total} bytes)", file=sys.stderr, end="\r")
    else:
        print(f"{prefix}: {transferred} bytes", file=sys.stderr)


def _download_part03_xml(dest: Path) -> None:
    print(f"Downloading Part 03 XML from {PART03_URL} ...", file=sys.stderr)
    req = urllib.request.Request(PART03_URL, headers={"User-Agent": "dicom-table-extractor"})
    with urllib.request.urlopen(req, timeout=60) as response:
        total = response.length
        tmp_path = dest.with_suffix(dest.suffix + ".tmp")
        transferred = 0
        with tmp_path.open("wb") as fh:
            while True:
                chunk = response.read(1 << 20)
                if not chunk:
                    break
                fh.write(chunk)
                transferred += len(chunk)
                _report_progress("Download", transferred, total)
        tmp_path.replace(dest)
    print(f"Saved Part 03 XML to {dest}", file=sys.stderr)


def _ensure_part03_xml(script_dir: Path, xml_arg: str | None) -> Path:
    xml_path = Path(xml_arg).resolve() if xml_arg else script_dir / "part03.xml"
    if xml_path.exists() or xml_arg:
        return xml_path

    script_dir.mkdir(parents=True, exist_ok=True)
    try:
        _download_part03_xml(xml_path)
    except (urllib.error.URLError, OSError) as exc:
        raise RuntimeError(
            f"Failed to download Part 03 XML from {PART03_URL}: {exc}"
        ) from exc
    return xml_path


def main(argv: Sequence[str]) -> int:
    script_dir = Path(__file__).resolve().parent
    xml_arg = argv[1] if len(argv) > 1 else None
    xml_path = _ensure_part03_xml(script_dir, xml_arg)
    output_path = Path(argv[2]).resolve() if len(argv) > 2 else script_dir / "_specific_character_sets.tsv"
    print(f"Parsing Specific Character Set tables from {xml_path} ...", file=sys.stderr)
    rows = build_specific_character_set_rows(xml_path)
    print(f"Extracted {len(rows)} rows", file=sys.stderr)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    print(f"Writing TSV to {output_path} ...", file=sys.stderr)
    with output_path.open("w", encoding="utf-8", newline="") as fh:
        writer = csv.writer(fh, delimiter="\t", lineterminator="\n")
        writer.writerow(OUTPUT_COLUMNS)
        for row in rows:
            writer.writerow([row.get(col, "") for col in OUTPUT_COLUMNS])
    print("Done", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
