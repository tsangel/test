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

constexpr UidType DecodeUidType(std::string_view type) {{
    if (type == "Transfer Syntax") {{
        return UidType::TransferSyntax;
    }}
    if (type == "SOP Class") {{
        return UidType::SopClass;
    }}
    if (type == "Well-known SOP Instance") {{
        return UidType::WellKnownSopInstance;
    }}
    if (type == "Meta SOP Class") {{
        return UidType::MetaSopClass;
    }}
    if (type == "Application Context Name") {{
        return UidType::ApplicationContextName;
    }}
    if (type == "Application Hosting Model") {{
        return UidType::ApplicationHostingModel;
    }}
    if (type == "Coding Scheme") {{
        return UidType::CodingScheme;
    }}
    if (type == "DICOM UIDs as a Coding Scheme") {{
        return UidType::DicomUidsAsCodingScheme;
    }}
    if (type == "Mapping Resource") {{
        return UidType::MappingResource;
    }}
    if (type == "Service Class") {{
        return UidType::ServiceClass;
    }}
    if (type == "Synchronization Frame of Reference") {{
        return UidType::SynchronizationFrameOfReference;
    }}
    if (type == "LDAP OID") {{
        return UidType::LdapOid;
    }}
    return UidType::Other;
}}

struct UidEntry {{
    std::string_view value;
    std::string_view name;
    std::string_view keyword;
    std::string_view type;
    UidType uid_type = DecodeUidType(type);
}};

constexpr std::array<UidEntry, {count}> kUidRegistry = {{{{
"""

FOOTER = """}};

} // namespace dicom
"""


def escape(value: str) -> str:
    return value.replace("\\", r"\\").replace('"', r"\"")


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


def write_header(rows: list[list[str]], dest: Path, source: Path) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    with dest.open("w", encoding="utf-8") as fh:
        fh.write(HEADER_TEMPLATE.format(source=source.as_posix(), count=len(rows)))
        for idx, (value, name, keyword, uid_type) in enumerate(rows):
            fh.write(
                f'    UidEntry{{"{escape(value)}", "{escape(name)}", "{escape(keyword)}", "{escape(uid_type)}"}},\n'
            )
        fh.write(FOOTER)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path("misc/dictionary/_uid_registry.tsv"))
    parser.add_argument("--output", type=Path, default=Path("include/uid_registry.hpp"))
    args = parser.parse_args()

    rows = load_rows(args.source)
    write_header(rows, args.output, args.source)


if __name__ == "__main__":
    main()
