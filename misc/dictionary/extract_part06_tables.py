#!/usr/bin/env python3

from __future__ import annotations

import re
import sys
import urllib.error
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Iterable, Iterator, List, Sequence


DB_NS = {"db": "http://docbook.org/ns/docbook"}
XML_ID_ATTR = "{http://www.w3.org/XML/1998/namespace}id"
TAG_TABLE_IDS: Sequence[str] = ("table_6-1", "table_7-1", "table_8-1")
UID_TABLE_ID = "table_A-1"
TAG_PATTERN = re.compile(r"^\([0-9A-FX]{4},[0-9A-FX]{4}\)$", flags=re.IGNORECASE)
ZERO_WIDTH = dict.fromkeys(map(ord, "\u200b\u200c\u200d"), None)
PART06_URL = "http://dicom.nema.org/medical/dicom/current/source/docbook/part06/part06.xml"


def _iter_tables(root: ET.Element, table_ids: Iterable[str]) -> Iterator[ET.Element]:
    lookup = set(table_ids)
    for table in root.findall(".//db:table", DB_NS):
        table_id = table.get(XML_ID_ATTR)
        if table_id in lookup:
            yield table
        if len(lookup) == 1 and table_id in lookup:
            return


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


def _extract_tag_row(cells: List[str]) -> list[str] | None:
    if not cells:
        return None
    tag = cells[0].replace(" ", "")
    if not TAG_PATTERN.match(tag):
        return None
    name = cells[1] if len(cells) > 1 else ""
    keyword = cells[2] if len(cells) > 2 else ""
    vr = cells[3] if len(cells) > 3 else ""
    vm = cells[4] if len(cells) > 4 else ""
    retired_text = cells[5] if len(cells) > 5 else ""
    return [tag, name, keyword, vr, vm, retired_text]


def _extract_uid_row(cells: List[str]) -> list[str] | None:
    if len(cells) < 4:
        return None
    uid_value, name, keyword, uid_type = cells[:4]
    if not uid_value:
        return None
    return [uid_value, name, keyword, uid_type]


def build_tag_table(xml_path: Path) -> list[list[str]]:
    tree = ET.parse(xml_path)
    root = tree.getroot()
    rows: list[list[str]] = []
    for table in _iter_tables(root, TAG_TABLE_IDS):
        tbody = table.find("db:tbody", DB_NS)
        if tbody is None:
            continue
        for tr in tbody.findall("db:tr", DB_NS):
            cells = [_text_content(td) for td in tr.findall("db:td", DB_NS)]
            row = _extract_tag_row(cells)
            if row:
                rows.append(row)
    return rows


def build_uid_registry(xml_path: Path) -> list[list[str]]:
    tree = ET.parse(xml_path)
    root = tree.getroot()
    table = next(_iter_tables(root, (UID_TABLE_ID,)), None)
    if table is None:
        return []
    tbody = table.find("db:tbody", DB_NS)
    if tbody is None:
        return []
    rows: list[list[str]] = []
    for tr in tbody.findall("db:tr", DB_NS):
        cells = [_text_content(td) for td in tr.findall("db:td", DB_NS)]
        row = _extract_uid_row(cells)
        if row:
            rows.append(row)
    return rows


def _report_progress(prefix: str, transferred: int, total: int | None) -> None:
    if total and total > 0:
        percent = transferred / total * 100
        print(f"{prefix}: {percent:5.1f}% ({transferred}/{total} bytes)", file=sys.stderr, end='\r')
    else:
        print(f"{prefix}: {transferred} bytes", file=sys.stderr)


def _download_part06_xml(dest: Path) -> None:
    print(f"Downloading Part 06 XML from {PART06_URL} ...", file=sys.stderr)
    req = urllib.request.Request(PART06_URL, headers={"User-Agent": "dicom-table-extractor"})
    with urllib.request.urlopen(req, timeout=60) as response:
        total = response.length
        tmp_path = dest.with_suffix(".xml.tmp")
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
    print(f"Saved Part 06 XML to {dest}", file=sys.stderr)


def _ensure_part06_xml(script_dir: Path, xml_arg: str | None) -> Path:
    xml_path = Path(xml_arg).resolve() if xml_arg else script_dir / "part06.xml"
    if xml_path.exists() or xml_arg:
        return xml_path

    script_dir.mkdir(parents=True, exist_ok=True)
    try:
        _download_part06_xml(xml_path)
    except (urllib.error.URLError, OSError) as exc:
        raise RuntimeError(
            f"Failed to download Part 06 XML from {PART06_URL}: {exc}"
        ) from exc
    return xml_path


def main(argv: Sequence[str]) -> int:
    script_dir = Path(__file__).resolve().parent
    xml_arg = argv[1] if len(argv) > 1 else None
    xml_path = _ensure_part06_xml(script_dir, xml_arg)
    tag_output = (
        Path(argv[2]).resolve()
        if len(argv) > 2
        else script_dir / "_dataelement_registry.txt"
    )
    uid_output = (
        Path(argv[3]).resolve()
        if len(argv) > 3
        else script_dir / "_uid_registry.txt"
    )

    print(f"Parsing tag tables from {xml_path} ...", file=sys.stderr)
    tag_rows = build_tag_table(xml_path)
    print(f"Parsed {len(tag_rows)} tag rows", file=sys.stderr)
    print(f"Parsing UID registry from {xml_path} ...", file=sys.stderr)
    uid_rows = build_uid_registry(xml_path)
    print(f"Parsed {len(uid_rows)} UID rows", file=sys.stderr)

    tag_output.parent.mkdir(parents=True, exist_ok=True)
    print(f"Writing tag table to {tag_output} ...", file=sys.stderr)
    with tag_output.open("w", encoding="utf-8") as fh:
        for tag, name, keyword, vr, vm, retired in tag_rows:
            print("\t".join((tag, name, keyword, vr, vm, retired)), file=fh)

    uid_output.parent.mkdir(parents=True, exist_ok=True)
    print(f"Writing UID registry to {uid_output} ...", file=sys.stderr)
    with uid_output.open("w", encoding="utf-8") as fh:
        for uid_value, name, keyword, uid_type in uid_rows:
            print("\t".join((uid_value, name, keyword, uid_type)), file=fh)
    print("Done", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
