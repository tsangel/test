#!/usr/bin/env python3
"""Generate include/uid_registry.hpp from _uid_registry.tsv."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path


HEADER_TEMPLATE = """// Auto-generated from {source}
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace dicom {{

enum class UidType : std::uint8_t {{
    TransferSyntax,
    SopClass,
    WellKnownSopInstance,
    MetaSopClass,
    ApplicationContextName,
    ApplicationHostingModel,
    CodingScheme,
    DicomUidsAsCodingScheme,
    MappingResource,
    ServiceClass,
    SynchronizationFrameOfReference,
    LdapOid,
    Other,
}};

struct UidEntry {{
    std::string_view value;
    std::string_view name;
    std::string_view keyword;
    std::string_view type;
    UidType uid_type;
}};

constexpr std::array<UidEntry, {count}> kUidRegistry = {{{{
"""

FOOTER_TEMPLATE = """}}}};

// Highest registry index whose UidType is TransferSyntax (auto-generated hint for fast tables).
constexpr std::size_t kMaxTransferSyntaxIndex = {max_transfer_index};

}} // namespace dicom
"""


def escape(value: str) -> str:
    return value.replace("\\", r"\\").replace('"', r"\"")


UID_TYPE_MAP = {
    "Transfer Syntax": "UidType::TransferSyntax",
    "SOP Class": "UidType::SopClass",
    "Well-known SOP Instance": "UidType::WellKnownSopInstance",
    "Meta SOP Class": "UidType::MetaSopClass",
    "Application Context Name": "UidType::ApplicationContextName",
    "Application Hosting Model": "UidType::ApplicationHostingModel",
    "Coding Scheme": "UidType::CodingScheme",
    "DICOM UIDs as a Coding Scheme": "UidType::DicomUidsAsCodingScheme",
    "Mapping Resource": "UidType::MappingResource",
    "Service Class": "UidType::ServiceClass",
    "Synchronization Frame of Reference": "UidType::SynchronizationFrameOfReference",
    "LDAP OID": "UidType::LdapOid",
}


def encode_uid_type(uid_type: str) -> str:
    return UID_TYPE_MAP.get(uid_type, "UidType::Other")


def load_rows(path: Path) -> list[list[str]]:
    rows: list[list[str]] = []
    with path.open("r", encoding="utf-8", newline="") as fh:
        reader = csv.reader(fh, delimiter="\t")
        for cols in reader:
            if not cols or cols[0].lower() == "uid_value":
                continue
            if len(cols) != 4:
                raise ValueError(f"expected 4 columns, got {len(cols)}")
            rows.append(cols)
    return rows


def write_header(
    rows: list[list[str]],
    dest: Path,
    source: Path,
    max_transfer_index: int,
) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    with dest.open("w", encoding="utf-8") as fh:
        fh.write(HEADER_TEMPLATE.format(source=source.as_posix(), count=len(rows)))
        for idx, (value, name, keyword, uid_type) in enumerate(rows):
            fh.write(
                f'    UidEntry{{"{escape(value)}", "{escape(name)}", "{escape(keyword)}", "{escape(uid_type)}", {encode_uid_type(uid_type)}}},\n'
            )
        fh.write(FOOTER_TEMPLATE.format(max_transfer_index=max_transfer_index))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path("misc/dictionary/_uid_registry.tsv"))
    parser.add_argument("--output", type=Path, default=Path("include/uid_registry.hpp"))
    args = parser.parse_args()

    rows = load_rows(args.source)

    max_transfer_index = -1
    for idx, (_value, _name, _keyword, uid_type) in enumerate(rows):
        if uid_type == "Transfer Syntax":
            max_transfer_index = idx

    if max_transfer_index < 0:
        raise ValueError("Transfer Syntax entries not found; cannot compute kMaxTransferSyntaxIndex")

    write_header(rows, args.output, args.source, max_transfer_index)


if __name__ == "__main__":
    main()
