#!/usr/bin/env python3

from __future__ import annotations

import argparse
import csv
import re
import sys
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable
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
IOD_COMPONENT_COLUMNS = (
    "iod_xml_id",
    "entry_index",
    "entry_kind",
    "ie_name",
    "component_name",
    "component_section_id",
    "usage",
    "usage_condition_text",
    "fg_scope",
    "source_table_xml_id",
)
COMPONENT_RULE_COLUMNS = (
    "component_section_id",
    "component_kind",
    "row_index",
    "path_signature",
    "nesting_level",
    "tag",
    "attribute_name",
    "keyword",
    "type",
    "description_text",
    "may_be_present_otherwise",
    "origin_section_id",
    "origin_table_xml_id",
)
OVERRIDE_COLUMNS = (
    "iod_xml_id",
    "path_signature",
    "tag",
    "effective_type_override",
    "override_condition_text",
    "source_section_id",
    "reason",
)
CONTEXT_COLUMNS = (
    "component_section_id",
    "context_key",
    "context_name",
    "context_kind",
    "is_root",
    "is_recursive",
)
CONTEXT_TRANSITION_COLUMNS = (
    "component_section_id",
    "from_context_key",
    "sequence_tag",
    "next_context_key",
    "transition_condition_text",
    "push_path",
    "is_recursive",
)
CONTEXT_RULE_INDEX_COLUMNS = (
    "component_section_id",
    "context_key",
    "rule_row_index",
)

TAG_PATTERN = re.compile(r"^\([0-9A-Fa-f]{4},[0-9A-Fa-f]{4}\)$")
TYPE_PATTERN = re.compile(r"\b(1C|2C|1|2|3)\b")


@dataclass(frozen=True)
class TagInfo:
    keyword: str
    vr: str


@dataclass
class TableInfo:
    table_id: str
    caption: str
    owner_section_id: str
    owner_section_title: str
    table: ET.Element


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--part03", default=None, help="Path to PS3.3 DocBook XML")
    parser.add_argument(
        "--tag-registry",
        type=Path,
        default=Path("misc/dictionary/_dataelement_registry.tsv"),
    )
    parser.add_argument(
        "--iod-component-output",
        type=Path,
        default=Path("misc/dictionary/_iod_component_registry.tsv"),
    )
    parser.add_argument(
        "--component-rules-output",
        type=Path,
        default=Path("misc/dictionary/_component_attribute_rules.tsv"),
    )
    parser.add_argument(
        "--overrides-output",
        type=Path,
        default=Path("misc/dictionary/_iod_attribute_overrides.tsv"),
    )
    parser.add_argument(
        "--context-output",
        type=Path,
        default=Path("misc/dictionary/_storage_context_registry.tsv"),
    )
    parser.add_argument(
        "--context-transition-output",
        type=Path,
        default=Path("misc/dictionary/_storage_context_transition_registry.tsv"),
    )
    parser.add_argument(
        "--context-rule-index-output",
        type=Path,
        default=Path("misc/dictionary/_storage_context_rule_index_registry.tsv"),
    )
    parser.add_argument(
        "--overwrite-overrides",
        action="store_true",
        help="Overwrite overrides TSV instead of only creating it when absent",
    )
    return parser.parse_args()


def load_tag_registry(path: Path) -> dict[str, TagInfo]:
    registry: dict[str, TagInfo] = {}
    if not path.exists():
        return registry
    with path.open("r", encoding="utf-8", newline="") as fh:
        reader = csv.reader(fh, delimiter="\t")
        for cols in reader:
            if not cols or cols[0].strip().lower() == "tag":
                continue
            if len(cols) < 4:
                continue
            tag = cols[0].strip().upper()
            keyword = cols[2].strip()
            vr = cols[3].strip().upper()
            if tag:
                registry[tag] = TagInfo(keyword=keyword, vr=vr)
    return registry


def normalize_tag(tag_text: str) -> str:
    value = tag_text.strip().upper()
    if TAG_PATTERN.match(value):
        return value
    return ""


def compact_tag(tag_text: str) -> str:
    return tag_text.strip()[1:-1].replace(",", "").upper()


def parse_declared_type(type_text: str) -> str:
    match = TYPE_PATTERN.search(type_text)
    return match.group(1) if match else ""


def split_usage(raw_usage: str) -> tuple[str, str]:
    raw = raw_usage.strip()
    if not raw:
        return "", ""
    usage = raw[0] if raw[0] in {"M", "C", "U"} else ""
    if len(raw) == 1 or usage == "":
        return usage or raw, ""
    if raw.startswith(f"{usage} - "):
        return usage, raw[4:].strip()
    return usage, raw[1:].strip(" -")


def count_prefix_arrows(text: str) -> int:
    count = 0
    for char in text.lstrip():
        if char == ">":
            count += 1
        else:
            break
    return count


def strip_prefix_arrows(text: str) -> str:
    return text.lstrip().lstrip(">").strip()


def collapse_spaces(text: str) -> str:
    return " ".join(text.split())


def may_be_present_otherwise(description_text: str) -> str:
    return "Y" if "may be present otherwise" in description_text.lower() else "N"


def attribute_table_shape(table: ET.Element) -> str | None:
    thead = table.find("db:thead", DB_NS)
    if thead is None:
        return None
    header_row = thead.find("db:tr", DB_NS)
    if header_row is None:
        return None
    headers = [text_content(th).strip().lower() for th in header_row.findall("db:th", DB_NS)]
    if len(headers) >= 3 and headers[:2] == ["attribute name", "tag"]:
        if len(headers) >= 4 and headers[2] == "type" and headers[3] in {
            "attribute description",
            "description",
        }:
            return "typed"
        if headers[2] in {"attribute description", "description"}:
            return "untyped"
    return None


def is_attribute_table(table: ET.Element) -> bool:
    return attribute_table_shape(table) is not None


def attribute_table_has_type(table: ET.Element) -> bool:
    return attribute_table_shape(table) == "typed"


def build_section_index(root: ET.Element) -> dict[str, ET.Element]:
    sections: dict[str, ET.Element] = {}
    for section in root.findall(".//db:section", DB_NS):
        section_id = section.get(XML_ID_ATTR, "").strip()
        if section_id:
            sections[section_id] = section
    return sections


def build_table_index(
    root: ET.Element, parent_map: dict[ET.Element, ET.Element]
) -> tuple[dict[str, TableInfo], dict[str, list[TableInfo]]]:
    by_id: dict[str, TableInfo] = {}
    by_section: dict[str, list[TableInfo]] = defaultdict(list)
    for table in root.findall(".//db:table", DB_NS):
        table_id = table.get(XML_ID_ATTR, "").strip()
        if not table_id:
            continue
        owner_section = nearest_ancestor(table, parent_map, "section")
        owner_section_id = owner_section.get(XML_ID_ATTR, "").strip() if owner_section is not None else ""
        info = TableInfo(
            table_id=table_id,
            caption=text_content(table.find("db:caption", DB_NS)),
            owner_section_id=owner_section_id,
            owner_section_title=title_text(owner_section),
            table=table,
        )
        by_id[table_id] = info
        by_section[owner_section_id].append(info)
    return by_id, by_section


def find_iod_section_id(
    section: ET.Element | None,
    parent_map: dict[ET.Element, ET.Element],
) -> str:
    excluded_title_parts = (
        "IOD Module Table",
        "Functional Group Macros",
        "IOD Description",
        "IOD Entity-Relationship Model",
        "Content Constraints",
    )
    current = section
    while current is not None:
        title = title_text(current)
        if "IOD" in title and not any(part in title for part in excluded_title_parts):
            return current.get(XML_ID_ATTR, "").strip()
        current = nearest_ancestor(current, parent_map, "section")
    return ""


def build_component_registry(
    root: ET.Element,
    parent_map: dict[ET.Element, ET.Element],
) -> list[list[str]]:
    entry_index_by_iod: dict[str, int] = defaultdict(int)
    rows: list[list[str]] = []
    for table in root.findall(".//db:table", DB_NS):
        table_id = table.get(XML_ID_ATTR, "").strip()
        caption = text_content(table.find("db:caption", DB_NS))
        if caption.endswith("IOD Modules"):
            entry_kind = "module"
        elif caption.endswith("Functional Group Macros"):
            entry_kind = "functional_group"
        else:
            continue

        owner_section = nearest_ancestor(table, parent_map, "section")
        iod_xml_id = find_iod_section_id(owner_section, parent_map)
        if not iod_xml_id:
            continue

        tbody = table.find("db:tbody", DB_NS)
        if tbody is None:
            continue

        current_ie = ""
        for tr in tbody.findall("db:tr", DB_NS):
            cells = tr.findall("db:td", DB_NS)
            if entry_kind == "module":
                if len(cells) == 4:
                    current_ie = text_content(cells[0])
                    component_cell = cells[1]
                    ref_cell = cells[2]
                    usage_cell = cells[3]
                elif len(cells) == 3:
                    component_cell = cells[0]
                    ref_cell = cells[1]
                    usage_cell = cells[2]
                else:
                    continue
                ie_name = current_ie
            else:
                if len(cells) != 3:
                    continue
                ie_name = ""
                component_cell = cells[0]
                ref_cell = cells[1]
                usage_cell = cells[2]

            component_name = text_content(component_cell)
            component_section_id = first_link_target(ref_cell)
            raw_usage = text_content(usage_cell)
            usage, usage_condition = split_usage(raw_usage)
            if not component_name or not component_section_id:
                continue
            rows.append(
                [
                    iod_xml_id,
                    str(entry_index_by_iod[iod_xml_id]),
                    entry_kind,
                    ie_name,
                    component_name,
                    component_section_id,
                    usage,
                    usage_condition,
                    "none",
                    table_id,
                ]
            )
            entry_index_by_iod[iod_xml_id] += 1
    return rows


def unique_component_sequence(component_rows: Iterable[list[str]]) -> list[tuple[str, str]]:
    seen: set[tuple[str, str]] = set()
    ordered: list[tuple[str, str]] = []
    for row in component_rows:
        component_section_id = row[5]
        component_kind = row[2]
        key = (component_section_id, component_kind)
        if key not in seen:
            seen.add(key)
            ordered.append(key)
    return ordered


def choose_primary_attribute_table(
    section_id: str,
    tables_by_section: dict[str, list[TableInfo]],
    section_index: dict[str, ET.Element],
) -> TableInfo | None:
    for table_info in tables_by_section.get(section_id, []):
        if is_attribute_table(table_info.table):
            return table_info
    section = section_index.get(section_id)
    if section is not None:
        for descendant in section.findall(".//db:table", DB_NS):
            if is_attribute_table(descendant):
                descendant_id = descendant.get(XML_ID_ATTR, "").strip()
                if descendant_id:
                    return TableInfo(
                        table_id=descendant_id,
                        caption=text_content(descendant.find("db:caption", DB_NS)),
                        owner_section_id=section_id,
                        owner_section_title=title_text(section),
                        table=descendant,
                    )
    return None


def is_sequence_row(tag: str, attribute_name: str, registry: dict[str, TagInfo]) -> bool:
    info = registry.get(tag)
    if info is not None and info.vr == "SQ":
        return True
    return attribute_name.strip().endswith("Sequence")


def parse_include_target(cells: list[ET.Element]) -> tuple[int, str, str] | None:
    if not cells:
        return None
    raw_text = text_content(cells[0]).strip()
    if not raw_text:
        return None
    if not strip_prefix_arrows(raw_text).startswith("Include"):
        return None
    target_table_id = ""
    for cell in cells:
        target_table_id = first_link_target(cell)
        if target_table_id.startswith("table_"):
            break
    if not target_table_id.startswith("table_"):
        return None
    include_text = collapse_spaces(" ".join(text_content(cell) for cell in cells if text_content(cell)))
    include_text = strip_prefix_arrows(include_text)
    return count_prefix_arrows(raw_text), target_table_id, include_text


def parse_attribute_row(
    cells: list[ET.Element],
    *,
    has_type: bool,
) -> tuple[int, str, str, str, str] | None:
    min_cells = 4 if has_type else 3
    if len(cells) < min_cells:
        return None
    raw_name = text_content(cells[0])
    tag = normalize_tag(text_content(cells[1]))
    declared_type = parse_declared_type(text_content(cells[2])) if has_type else "unknown"
    if not raw_name or not tag:
        return None
    if has_type and not declared_type:
        return None
    depth = count_prefix_arrows(raw_name)
    attribute_name = strip_prefix_arrows(raw_name)
    description_start = 3 if has_type else 2
    description_text = " ".join(
        text_content(cell) for cell in cells[description_start:] if text_content(cell)
    )
    return depth, attribute_name, tag, declared_type, description_text


def build_component_attribute_rules(
    component_rows: list[list[str]],
    tables_by_id: dict[str, TableInfo],
    tables_by_section: dict[str, list[TableInfo]],
    section_index: dict[str, ET.Element],
    tag_registry: dict[str, TagInfo],
) -> tuple[list[list[str]], list[str], list[str]]:
    rows: list[list[str]] = []
    missing_sections: set[str] = set()
    include_cycles: set[str] = set()

    for component_section_id, component_kind in unique_component_sequence(component_rows):
        primary_table = choose_primary_attribute_table(
            component_section_id,
            tables_by_section,
            section_index,
        )
        if primary_table is None:
            missing_sections.add(component_section_id)
            continue

        row_index = 0

        def expand_table(
            table_id: str,
            depth_offset: int,
            active_ancestors: list[str],
            include_stack: list[str],
        ) -> None:
            nonlocal row_index
            table_info = tables_by_id.get(table_id)
            if table_info is None:
                print(f"warning: missing included table {table_id}", file=sys.stderr)
                return
            if table_id in include_stack:
                include_cycles.add(table_id)
                return

            tbody = table_info.table.find("db:tbody", DB_NS)
            if tbody is None:
                return
            has_type = attribute_table_has_type(table_info.table)

            current_ancestors = list(active_ancestors)
            for tr in tbody.findall("db:tr", DB_NS):
                cells = tr.findall("db:td", DB_NS)
                if not cells:
                    continue
                include_info = parse_include_target(cells)
                if include_info is not None:
                    include_depth, include_table_id, _ = include_info
                    expand_table(
                        include_table_id,
                        depth_offset + include_depth,
                        list(current_ancestors),
                        include_stack + [table_id],
                    )
                    continue

                parsed = parse_attribute_row(cells, has_type=has_type)
                if parsed is None:
                    continue

                local_depth, attribute_name, tag, declared_type, description_text = parsed
                absolute_depth = depth_offset + local_depth
                if len(current_ancestors) > absolute_depth:
                    current_ancestors = current_ancestors[:absolute_depth]

                path_parts = list(current_ancestors) + [compact_tag(tag)]
                rows.append(
                    [
                        component_section_id,
                        component_kind,
                        str(row_index),
                        "/".join(path_parts),
                        str(absolute_depth),
                        tag,
                        attribute_name,
                        tag_registry.get(tag, TagInfo("", "")).keyword,
                        declared_type,
                        description_text,
                        may_be_present_otherwise(description_text),
                        table_info.owner_section_id,
                        table_id,
                    ]
                )
                row_index += 1

                if is_sequence_row(tag, attribute_name, tag_registry):
                    if len(current_ancestors) <= absolute_depth:
                        current_ancestors.append(compact_tag(tag))
                    else:
                        current_ancestors[absolute_depth] = compact_tag(tag)

        expand_table(primary_table.table_id, 0, [], [])
    return rows, sorted(missing_sections), sorted(include_cycles)


def build_storage_context_tables(
    component_rows: list[list[str]],
    tables_by_id: dict[str, TableInfo],
    tables_by_section: dict[str, list[TableInfo]],
    section_index: dict[str, ET.Element],
    tag_registry: dict[str, TagInfo],
    rule_rows: list[list[str]],
) -> tuple[list[list[str]], list[list[str]], list[list[str]]]:
    contexts: dict[tuple[str, str], dict[str, str]] = {}
    transitions: dict[tuple[str, str, str, str], dict[str, str]] = {}

    for component_section_id, component_kind in unique_component_sequence(component_rows):
        primary_table = choose_primary_attribute_table(
            component_section_id,
            tables_by_section,
            section_index,
        )
        if primary_table is None:
            continue

        visited_tables: set[str] = set()

        def ensure_context(
            table_id: str,
            *,
            is_root: bool = False,
            is_recursive: bool = False,
        ) -> None:
            table_info = tables_by_id.get(table_id)
            if table_info is None:
                return
            key = (component_section_id, table_id)
            if key not in contexts:
                contexts[key] = {
                    "component_section_id": component_section_id,
                    "context_key": table_id,
                    "context_name": table_info.caption,
                    "context_kind": "table",
                    "is_root": "Y" if is_root else "N",
                    "is_recursive": "Y" if is_recursive else "N",
                }
                return
            if is_root:
                contexts[key]["is_root"] = "Y"
            if is_recursive:
                contexts[key]["is_recursive"] = "Y"

        def record_transition(
            from_table_id: str,
            sequence_tag: str,
            next_table_id: str,
            transition_condition_text: str,
            *,
            push_path: bool,
            is_recursive: bool,
        ) -> None:
            transition_key = (
                component_section_id,
                from_table_id,
                sequence_tag,
                next_table_id,
            )
            existing = transitions.get(transition_key)
            if existing is None:
                transitions[transition_key] = {
                    "component_section_id": component_section_id,
                    "from_context_key": from_table_id,
                    "sequence_tag": sequence_tag,
                    "next_context_key": next_table_id,
                    "transition_condition_text": transition_condition_text,
                    "push_path": "Y" if push_path else "N",
                    "is_recursive": "Y" if is_recursive else "N",
                }
                return
            if transition_condition_text and not existing["transition_condition_text"]:
                existing["transition_condition_text"] = transition_condition_text
            if push_path:
                existing["push_path"] = "Y"
            if is_recursive:
                existing["is_recursive"] = "Y"

        def visit_table(
            table_id: str,
            active_ancestors: list[str],
            include_stack: list[str],
        ) -> None:
            table_info = tables_by_id.get(table_id)
            if table_info is None:
                return
            ensure_context(table_id, is_root=(table_id == primary_table.table_id))
            if table_id in visited_tables:
                return
            visited_tables.add(table_id)

            tbody = table_info.table.find("db:tbody", DB_NS)
            if tbody is None:
                return
            has_type = attribute_table_has_type(table_info.table)
            current_ancestors = list(active_ancestors)

            for tr in tbody.findall("db:tr", DB_NS):
                cells = tr.findall("db:td", DB_NS)
                if not cells:
                    continue
                include_info = parse_include_target(cells)
                if include_info is not None:
                    _, include_table_id, include_text = include_info
                    sequence_tag = current_ancestors[-1] if current_ancestors else ""
                    is_recursive = include_table_id == table_id or include_table_id in include_stack
                    ensure_context(include_table_id, is_recursive=is_recursive)
                    if is_recursive:
                        ensure_context(table_id, is_recursive=True)
                    record_transition(
                        table_id,
                        sequence_tag,
                        include_table_id,
                        include_text,
                        push_path=bool(sequence_tag),
                        is_recursive=is_recursive,
                    )
                    if not is_recursive:
                        visit_table(include_table_id, list(current_ancestors), include_stack + [table_id])
                    continue

                parsed = parse_attribute_row(cells, has_type=has_type)
                if parsed is None:
                    continue
                local_depth, attribute_name, tag, _, _ = parsed
                if len(current_ancestors) > local_depth:
                    current_ancestors = current_ancestors[:local_depth]
                if is_sequence_row(tag, attribute_name, tag_registry):
                    compact = compact_tag(tag)
                    if len(current_ancestors) <= local_depth:
                        current_ancestors.append(compact)
                    else:
                        current_ancestors[local_depth] = compact

        visit_table(primary_table.table_id, [], [])

    context_rows = sorted(
        contexts.values(),
        key=lambda row: (
            row["component_section_id"],
            row["is_root"] != "Y",
            row["context_key"],
        ),
    )

    transition_rows = sorted(
        transitions.values(),
        key=lambda row: (
            row["component_section_id"],
            row["from_context_key"],
            row["sequence_tag"],
            row["next_context_key"],
        ),
    )

    context_keys = {(row["component_section_id"], row["context_key"]) for row in context_rows}
    context_rule_rows: list[list[str]] = []
    for rule_row in rule_rows:
        component_section_id = rule_row[0]
        origin_table_xml_id = rule_row[12]
        if (component_section_id, origin_table_xml_id) not in context_keys:
            continue
        context_rule_rows.append(
            [
                component_section_id,
                origin_table_xml_id,
                rule_row[2],
            ]
        )
    context_rule_rows.sort(key=lambda row: (row[0], row[1], int(row[2])))

    return (
        [
            [
                row["component_section_id"],
                row["context_key"],
                row["context_name"],
                row["context_kind"],
                row["is_root"],
                row["is_recursive"],
            ]
            for row in context_rows
        ],
        [
            [
                row["component_section_id"],
                row["from_context_key"],
                row["sequence_tag"],
                row["next_context_key"],
                row["transition_condition_text"],
                row["push_path"],
                row["is_recursive"],
            ]
            for row in transition_rows
        ],
        context_rule_rows,
    )


def write_tsv(columns: tuple[str, ...], rows: list[list[str]], output: Path) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", encoding="utf-8", newline="") as fh:
        writer = csv.writer(fh, delimiter="\t", lineterminator="\n")
        writer.writerow(columns)
        writer.writerows(rows)


def ensure_overrides_scaffold(path: Path, overwrite: bool) -> None:
    if path.exists() and not overwrite:
        return
    write_tsv(OVERRIDE_COLUMNS, [], path)


def main() -> int:
    args = parse_args()
    script_dir = Path(__file__).resolve().parent
    part03_path = ensure_xml(script_dir, args.part03, "part03.xml", PART03_URL)
    tag_registry = load_tag_registry(args.tag_registry)

    print(f"Parsing IOD registry tables from {part03_path} ...", file=sys.stderr)
    root = ET.parse(part03_path).getroot()
    parent_map = build_parent_map(root)
    section_index = build_section_index(root)
    tables_by_id, tables_by_section = build_table_index(root, parent_map)

    component_rows = build_component_registry(root, parent_map)
    rule_rows, missing_sections, include_cycles = build_component_attribute_rules(
        component_rows,
        tables_by_id,
        tables_by_section,
        section_index,
        tag_registry,
    )
    context_rows, context_transition_rows, context_rule_rows = build_storage_context_tables(
        component_rows,
        tables_by_id,
        tables_by_section,
        section_index,
        tag_registry,
        rule_rows,
    )
    unknown_type_rows = sum(1 for row in rule_rows if row[8] == "unknown")
    unknown_type_sections = len({row[0] for row in rule_rows if row[8] == "unknown"})

    print(f"Parsed {len(component_rows)} IOD component rows", file=sys.stderr)
    print(f"Parsed {len(rule_rows)} component attribute rows", file=sys.stderr)
    print(
        "Parsed "
        f"{len(context_rows)} storage contexts, "
        f"{len(context_transition_rows)} storage context transitions, "
        f"{len(context_rule_rows)} storage context rule indices",
        file=sys.stderr,
    )
    if unknown_type_rows:
        print(
            "note: preserved "
            f"{unknown_type_rows} rows from {unknown_type_sections} component sections "
            "without a PS3.3 Type column; exported type=unknown",
            file=sys.stderr,
        )
    if include_cycles:
        print(
            "warning: include cycles skipped at "
            + ", ".join(include_cycles[:8])
            + (" ..." if len(include_cycles) > 8 else ""),
            file=sys.stderr,
        )
    if missing_sections:
        print(
            "warning: no attribute table found for "
            f"{len(missing_sections)} component sections: "
            + ", ".join(missing_sections[:12])
            + (" ..." if len(missing_sections) > 12 else ""),
            file=sys.stderr,
        )

    write_tsv(IOD_COMPONENT_COLUMNS, component_rows, args.iod_component_output)
    write_tsv(COMPONENT_RULE_COLUMNS, rule_rows, args.component_rules_output)
    write_tsv(CONTEXT_COLUMNS, context_rows, args.context_output)
    write_tsv(CONTEXT_TRANSITION_COLUMNS, context_transition_rows, args.context_transition_output)
    write_tsv(CONTEXT_RULE_INDEX_COLUMNS, context_rule_rows, args.context_rule_index_output)
    ensure_overrides_scaffold(args.overrides_output, args.overwrite_overrides)

    print(f"Wrote {args.iod_component_output}", file=sys.stderr)
    print(f"Wrote {args.component_rules_output}", file=sys.stderr)
    print(f"Wrote {args.context_output}", file=sys.stderr)
    print(f"Wrote {args.context_transition_output}", file=sys.stderr)
    print(f"Wrote {args.context_rule_index_output}", file=sys.stderr)
    print(f"Ensured {args.overrides_output}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
