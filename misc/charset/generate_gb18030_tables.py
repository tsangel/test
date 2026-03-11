#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import urllib.request
import xml.etree.ElementTree as ET

from _generator_common import write_text_if_changed


ROOT = pathlib.Path(__file__).resolve().parents[2]
DATA_DIR = ROOT / "misc" / "charset" / "data" / "icu"
OUT_PATH = ROOT / "src" / "charset" / "generated" / "gb18030_tables.hpp"

SOURCE_URL = (
    "https://raw.githubusercontent.com/unicode-org/icu-data/main/"
    "charset/data/xml/gb-18030-2000.xml"
)
SOURCE_FILENAME = "gb-18030-2000.xml"


def ensure_source_downloaded() -> pathlib.Path:
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    path = DATA_DIR / SOURCE_FILENAME
    if not path.exists():
        with urllib.request.urlopen(SOURCE_URL) as response:
            path.write_bytes(response.read())
    return path


def parse_xml_root(path: pathlib.Path) -> ET.Element:
    return ET.parse(path).getroot()


def to_18030_codepoint(text: str) -> int:
    parts = [int(part, 16) for part in text.split()]
    if len(parts) != 4:
        raise ValueError(f"expected 4-byte GB18030 sequence, got: {text}")

    b1, b2, b3, b4 = parts
    if b2 < 0x30 or b2 > 0x39 or b3 < 0x81 or b3 > 0xFE or b4 < 0x30 or b4 > 0x39:
        raise ValueError(f"out-of-range GB18030 sequence: {text}")

    if 0x90 <= b1 <= 0xE3:
        return (b4 - 0x30) + (b3 - 0x81) * 10 + (b2 - 0x30) * 1260 + (b1 - 0x90) * 12600 + 0x10000
    if 0x81 <= b1 <= 0x84:
        return (b4 - 0x30) + (b3 - 0x81) * 10 + (b2 - 0x30) * 1260 + (b1 - 0x81) * 12600
    raise ValueError(f"out-of-range GB18030 sequence: {text}")


def translate_map_from_root(root: ET.Element) -> tuple[dict[int, int], dict[int, int]]:
    assignments = root.find("assignments")
    if assignments is None:
        raise RuntimeError("missing <assignments> in gb18030 XML")

    mb_to_unicode: dict[int, int] = {}
    unicode_to_mb: dict[int, int] = {}
    for elem in assignments:
        if elem.tag != "a":
            continue
        unicode_value = int(elem.attrib["u"], 16)
        byte_parts = [int(part, 16) for part in elem.attrib["b"].split()]
        if len(byte_parts) > 2:
            continue
        multibyte = byte_parts[0] if len(byte_parts) == 1 else (byte_parts[0] << 8) | byte_parts[1]
        mb_to_unicode[multibyte] = unicode_value
        unicode_to_mb[unicode_value] = multibyte
    return mb_to_unicode, unicode_to_mb


def offset_table_from_root(root: ET.Element) -> list[tuple[int, int, int, int, int]]:
    assignments = root.find("assignments")
    if assignments is None:
        raise RuntimeError("missing <assignments> in gb18030 XML")

    offsets: dict[int, list[tuple[int, int]]] = {}
    for elem in assignments:
        if elem.tag != "a":
            continue
        if len(elem.attrib["b"].split()) != 4:
            continue
        unicode_value = int(elem.attrib["u"], 16)
        if unicode_value > 0xFFFF:
            continue
        codepoint = to_18030_codepoint(elem.attrib["b"])
        offset = unicode_value - codepoint
        offsets.setdefault(offset, []).append((unicode_value, codepoint))

    result: list[tuple[int, int, int, int, int]] = []
    for offset in sorted(offsets):
        unicode_values = [item[0] for item in offsets[offset]]
        codepoints = [item[1] for item in offsets[offset]]
        if unicode_values[-1] - unicode_values[0] != codepoints[-1] - codepoints[0]:
            raise RuntimeError(f"non-contiguous one-to-one GB18030 offset range: {offset}")
        if unicode_values[-1] - unicode_values[0] + 1 != len(unicode_values):
            raise RuntimeError(f"gapped one-to-one GB18030 offset range: {offset}")
        result.append((offset, unicode_values[0], unicode_values[-1], codepoints[0], codepoints[-1]))

    for elem in assignments:
        if elem.tag != "range":
            continue
        unicode_first = int(elem.attrib["uFirst"], 16)
        unicode_last = int(elem.attrib["uLast"], 16)
        if unicode_first > 0xFFFF:
            continue
        codepoint_first = to_18030_codepoint(elem.attrib["bFirst"])
        codepoint_last = to_18030_codepoint(elem.attrib["bLast"])
        if unicode_last - codepoint_last != unicode_first - codepoint_first:
            raise RuntimeError("GB18030 range offset mismatch")
        result.append(
            (unicode_last - codepoint_last, unicode_first, unicode_last, codepoint_first, codepoint_last)
        )

    result.sort(key=lambda item: item[1])
    return result


def bitsum_table() -> list[int]:
    return [bin(index).count("1") for index in range(256)]


def build_gb18030_to_unicode(root: ET.Element) -> list[int]:
    mb_to_unicode, _ = translate_map_from_root(root)
    values: list[int] = []
    for b1 in range(0x81, 0xFE + 1):
        for b2 in range(0x40, 0xFE + 1):
            if b2 == 0x7F:
                continue
            key = (b1 << 8) | b2
            if key not in mb_to_unicode:
                raise RuntimeError(f"missing GB18030 two-byte mapping for {key:04X}")
            values.append(mb_to_unicode[key])
    return values


def build_unicode_to_index(root: ET.Element) -> tuple[list[int], list[int]]:
    _, unicode_to_mb = translate_map_from_root(root)
    number_of_codes_in_page = [0] * 256
    for unicode_value in unicode_to_mb:
        number_of_codes_in_page[unicode_value >> 8] += 1

    page_size = 16 * 2
    page_exists = [1 if number_of_codes_in_page[index] else 0 for index in range(256)]

    index_table: list[int] = []
    for page in range(256):
        offset = sum(page_exists[:page]) * page_size + 256
        index_table.append(offset if page_exists[page] else 0)

    current_offset = 256
    code_index = 0
    for page in range(256):
        if not page_exists[page]:
            continue
        for nibble in range(16):
            unicode_first = (page << 8) + (nibble << 4)
            unicode_last = unicode_first + 16
            index_table.append(code_index)
            use_bits = 0
            for unicode_value in range(unicode_first, unicode_last):
                use_bits <<= 1
                if unicode_value in unicode_to_mb:
                    use_bits += 1
                    code_index += 1
            index_table.append(use_bits)
            current_offset += 2

    sorted_unicodes = sorted(unicode_to_mb)
    multibyte_table = [unicode_to_mb[unicode_value] for unicode_value in sorted_unicodes]
    return index_table, multibyte_table


def build_offset_ranges(root: ET.Element) -> tuple[list[int], list[int]]:
    ranges = offset_table_from_root(root)
    codepoint_ranges: list[int] = []
    unicode_ranges: list[int] = []
    for _, unicode_first, unicode_last, codepoint_first, codepoint_last in ranges:
        codepoint_ranges.extend((codepoint_first, codepoint_last))
        unicode_ranges.extend((unicode_first, unicode_last))
    return codepoint_ranges, unicode_ranges


def format_array(name: str, ctype: str, values: list[int], values_per_row: int, comment: str) -> list[str]:
    lines = [f"// {comment}", f"inline constexpr {ctype} {name}[{len(values)}] = {{"]
    for row_start in range(0, len(values), values_per_row):
        row = values[row_start : row_start + values_per_row]
        lines.append("    " + ", ".join(f"0x{value:x}" for value in row) + ",")
    lines.append("};")
    lines.append("")
    return lines


def generate_header() -> str:
    root = parse_xml_root(ensure_source_downloaded())
    gb18030_to_unicode = build_gb18030_to_unicode(root)
    unicode_to_index, index_to_multibyte = build_unicode_to_index(root)
    codepoint_ranges, unicode_ranges = build_offset_ranges(root)
    bit_sums = bitsum_table()

    lines = [
        "#pragma once",
        "",
        "// Auto-generated by misc/charset/generate_gb18030_tables.py",
        "// Source origin:",
        f"// - {SOURCE_URL}",
        "#include <cstdint>",
        "",
        "namespace dicom::charset::tables {",
        "",
    ]
    lines.extend(
        format_array(
            "map_gb18030_to_unicode", "std::uint16_t", gb18030_to_unicode, 10,
            "GBK/GB2312 two-byte area to Unicode",
        )
    )
    lines.extend(
        format_array(
            "map_gb18030_unicode_to_index", "std::uint16_t", unicode_to_index, 10,
            "Unicode page/index table for GBK/GB18030 reverse lookup",
        )
    )
    lines.extend(
        format_array(
            "map_gb18030_index_to_multibyte", "std::uint16_t", index_to_multibyte, 10,
            "Unicode reverse-lookup index to GBK multibyte value",
        )
    )
    lines.extend(
        format_array(
            "bitsum_table", "std::uint8_t", bit_sums, 16,
            "Population count lookup for reverse-lookup bitmasks",
        )
    )
    lines.extend(
        format_array(
            "gb18030_codepoint_range", "std::uint16_t", codepoint_ranges, 8,
            "Ranges of GB18030 four-byte code points for BMP mappings",
        )
    )
    lines.extend(
        format_array(
            "gb18030_unicode_range", "std::uint16_t", unicode_ranges, 8,
            "Ranges of BMP Unicode values for GB18030 four-byte mappings",
        )
    )
    lines.append("}  // namespace dicom::charset::tables")
    lines.append("")
    return "\n".join(lines)


def main() -> None:
    OUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    write_text_if_changed(OUT_PATH, generate_header())


if __name__ == "__main__":
    main()
