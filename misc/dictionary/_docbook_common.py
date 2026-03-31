from __future__ import annotations

from pathlib import Path
import sys
import urllib.error
import urllib.request
import xml.etree.ElementTree as ET


DB_NS = {"db": "http://docbook.org/ns/docbook"}
XML_ID_ATTR = "{http://www.w3.org/XML/1998/namespace}id"
ZERO_WIDTH = dict.fromkeys(map(ord, "\u200b\u200c\u200d"), None)


def text_content(element: ET.Element | None) -> str:
    if element is None:
        return ""
    parts: list[str] = []
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


def build_parent_map(root: ET.Element) -> dict[ET.Element, ET.Element]:
    parent_map: dict[ET.Element, ET.Element] = {}
    for parent in root.iter():
        for child in parent:
            parent_map[child] = parent
    return parent_map


def local_name(element: ET.Element) -> str:
    return element.tag.split("}", 1)[-1]


def nearest_ancestor(
    element: ET.Element,
    parent_map: dict[ET.Element, ET.Element],
    wanted_local_name: str,
) -> ET.Element | None:
    current = parent_map.get(element)
    while current is not None:
        if local_name(current) == wanted_local_name:
            return current
        current = parent_map.get(current)
    return None


def title_text(section: ET.Element | None) -> str:
    if section is None:
        return ""
    return text_content(section.find("db:title", DB_NS))


def first_link_target(element: ET.Element | None) -> str:
    if element is None:
        return ""
    xref = element.find(".//db:xref", DB_NS)
    if xref is not None:
        return xref.get("linkend", "").strip()
    olink = element.find(".//db:olink", DB_NS)
    if olink is not None:
        return olink.get("targetptr", "").strip()
    return ""


def find_table_by_id(root: ET.Element, table_id: str) -> ET.Element | None:
    for table in root.findall(".//db:table", DB_NS):
        if table.get(XML_ID_ATTR) == table_id:
            return table
    return None


def _report_progress(prefix: str, transferred: int, total: int | None) -> None:
    if total and total > 0:
        percent = transferred / total * 100
        print(
            f"{prefix}: {percent:5.1f}% ({transferred}/{total} bytes)",
            file=sys.stderr,
            end="\r",
        )
    else:
        print(f"{prefix}: {transferred} bytes", file=sys.stderr)


def download_xml(url: str, dest: Path) -> None:
    print(f"Downloading XML from {url} ...", file=sys.stderr)
    req = urllib.request.Request(url, headers={"User-Agent": "dicomsdl-docbook-extractor"})
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
    print(f"Saved XML to {dest}", file=sys.stderr)


def ensure_xml(script_dir: Path, xml_arg: str | None, default_name: str, url: str) -> Path:
    xml_path = Path(xml_arg).resolve() if xml_arg else script_dir / default_name
    if xml_path.exists():
        return xml_path

    script_dir.mkdir(parents=True, exist_ok=True)
    try:
        download_xml(url, xml_path)
    except (urllib.error.URLError, OSError) as exc:
        raise RuntimeError(f"Failed to download XML from {url}: {exc}") from exc
    return xml_path
