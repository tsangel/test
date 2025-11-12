#!/usr/bin/env python3
"""Generate keyword and tag lookup tables using CHD perfect hashes."""

from __future__ import annotations

import argparse
import math
import re
from dataclasses import dataclass
from pathlib import Path
from typing import List, Sequence

from generate_dataelement_registry import parse_rows

RET_YEAR_RE = re.compile(r"RET\s*\((\d{4})")
SENTINEL = 0xFFFF
GOLDEN_RATIO32 = 0x9E3779B1
HASH_OFFSET64 = 0x6A09E667F3BCC909
HASH_INCR64 = 0x9E3779B97F4A7C15
HASH_MIX1 = 0xBF58476D1CE4E5B9
HASH_MIX2 = 0x94D049BB133111EB
MASK64 = 0xFFFFFFFFFFFFFFFF


@dataclass(frozen=True)
class KeywordEntry:
    registry_index: int
    keyword: str
    tag: str
    vr: str
    tag_value: int | None


@dataclass(frozen=True)
class TagWildcardEntry:
    registry_index: int
    value: int
    mask: int
    retired: bool


@dataclass(frozen=True)
class ChdTables:
    seed: int
    table_size: int
    mask: int
    bucket_count: int
    slots: List[int]
    displacements: List[int]


def should_include(keyword: str, retired: str) -> bool:
    if not keyword:
        return False
    match = RET_YEAR_RE.search(retired)
    if match and int(match.group(1)) < 2010:
        return False
    return True


def is_retired_entry(retired: str) -> bool:
    return "RET" in retired.upper()


def mix64(value: int) -> int:
    value ^= value >> 30
    value = (value * HASH_MIX1) & MASK64
    value ^= value >> 27
    value = (value * HASH_MIX2) & MASK64
    value ^= value >> 31
    return value & MASK64


def base_hash64(text: str) -> int:
    value = HASH_OFFSET64
    for byte in text.encode("utf-8"):
        value = (value + byte + HASH_INCR64) & MASK64
        value = mix64(value)
    return value


def keyword_hash64(text: str, seed: int) -> int:
    value = base_hash64(text)
    return mix64(value ^ ((seed * GOLDEN_RATIO32) & MASK64))


def strip_tag_separators(tag: str) -> str:
    return tag.replace("(", "").replace(")", "").replace(",", "").replace(" ", "")


def parse_tag_pattern(tag: str) -> tuple[int, int, bool] | None:
    cleaned = strip_tag_separators(tag)
    if len(cleaned) != 8:
        return None
    value = 0
    mask = 0
    has_wildcard = False
    for ch in cleaned:
        value <<= 4
        mask <<= 4
        if "0" <= ch <= "9" or "A" <= ch <= "F" or "a" <= ch <= "f":
            digit = int(ch, 16)
            value |= digit
            mask |= 0xF
        elif ch in {"x", "X"}:
            has_wildcard = True
            # Wildcard nibble: mask stays zero, value already shifted.
        else:
            return None
    return value, mask, has_wildcard


def load_entries(source: Path) -> tuple[List[KeywordEntry], List[KeywordEntry], List[TagWildcardEntry]]:
    rows = parse_rows(source)
    keyword_entries: List[KeywordEntry] = []
    tag_entries: List[KeywordEntry] = []
    wildcard_entries: List[TagWildcardEntry] = []
    for idx, row in enumerate(rows):
        tag, _name, keyword, vr, _vm, retired = row
        keyword = keyword.strip()
        retired = retired.strip()
        retired_flag = is_retired_entry(retired)
        pattern = parse_tag_pattern(tag)
        tag_value: int | None = None
        if pattern is not None:
            value, mask, has_wildcard = pattern
            if has_wildcard:
                wildcard_entries.append(TagWildcardEntry(idx, value, mask, retired_flag))
            else:
                tag_value = value
        entry = KeywordEntry(idx, keyword, tag, vr, tag_value)
        if should_include(keyword, retired):
            keyword_entries.append(entry)
        if tag_value is not None:
            tag_entries.append(entry)
    return keyword_entries, tag_entries, wildcard_entries

def next_power_of_two(value: int) -> int:
    if value <= 0:
        return 1
    return 1 << (value - 1).bit_length()


def build_chd_from_hashes(
    entries: Sequence[KeywordEntry],
    hashed_values: Sequence[int],
    load_factor: float = 2.0,
) -> ChdTables:
    bucket_count = len(entries)
    if bucket_count == 0:
        raise ValueError("No entries available for CHD table generation")
    table_target = max(bucket_count + 1, int(math.ceil(bucket_count * load_factor)))
    table_size = next_power_of_two(table_target)
    mask = table_size - 1

    # Try deterministic set of seeds until a perfect placement is found.
    for seed in range(1, 1 << 16):
        seed_mix = (seed * GOLDEN_RATIO32) & MASK64
        mixed_hashes = [mix64(hv ^ seed_mix) for hv in hashed_values]
        buckets: List[List[int]] = [[] for _ in range(bucket_count)]
        for idx, mixed in enumerate(mixed_hashes):
            bucket = mixed % bucket_count
            buckets[bucket].append(idx)
        order = sorted(range(bucket_count), key=lambda b: (len(buckets[b]), b), reverse=True)
        slots = [SENTINEL] * table_size
        displacements = [0] * bucket_count
        success = True
        for bucket_idx in order:
            bucket_entries = buckets[bucket_idx]
            if not bucket_entries:
                continue
            displacement = 0 if len(bucket_entries) == 1 else 1
            placed_slots: List[int] = []
            while displacement < table_size:
                placed_slots.clear()
                conflict = False
                for entry_idx in bucket_entries:
                    slot = (mixed_hashes[entry_idx] + displacement) & mask
                    if slots[slot] != SENTINEL or slot in placed_slots:
                        conflict = True
                        break
                    placed_slots.append(slot)
                if not conflict:
                    break
                displacement += 1
            if displacement >= table_size:
                success = False
                break
            displacements[bucket_idx] = displacement
            for entry_idx, slot in zip(bucket_entries, placed_slots):
                slots[slot] = entries[entry_idx].registry_index
        if success:
            return ChdTables(seed, table_size, mask, bucket_count, slots, displacements)
    raise RuntimeError("Unable to build CHD tables with the given configuration")


def build_keyword_chd(entries: Sequence[KeywordEntry], load_factor: float = 2.0) -> ChdTables:
    hashed_values = [base_hash64(entry.keyword) for entry in entries]
    return build_chd_from_hashes(entries, hashed_values, load_factor)


def tag_base_hash(value: int) -> int:
    hashed = HASH_OFFSET64
    for shift in range(0, 32, 8):
        byte = (value >> shift) & 0xFF
        hashed = (hashed + byte + HASH_INCR64) & MASK64
        hashed = mix64(hashed)
    return hashed


def build_tag_chd(entries: Sequence[KeywordEntry], load_factor: float = 2.0) -> ChdTables:
    hashed_values: List[int] = []
    for entry in entries:
        if entry.tag_value is None:
            raise ValueError("Tag entry missing numeric value")
        hashed_values.append(tag_base_hash(entry.tag_value))
    return build_chd_from_hashes(entries, hashed_values, load_factor)




def escape(value: str) -> str:
    return value.replace("\\", r"\\").replace('"', r"\"")


def format_array(values: Sequence[str], indent: str = "    ", per_line: int = 4) -> List[str]:
    lines: List[str] = []
    for i in range(0, len(values), per_line):
        chunk = ", ".join(values[i : i + per_line])
        lines.append(f"{indent}{chunk},")
    return lines


def render(
    entries: Sequence[KeywordEntry],
    keyword_chd: ChdTables,
    tag_entries: Sequence[KeywordEntry],
    tag_chd: ChdTables,
    wildcard_entries: Sequence[TagWildcardEntry],
) -> str:
    lines: List[str] = []
    lines.append("// Auto-generated keyword lookup tables. Do not edit manually.")
    lines.append("#pragma once")
    lines.append("")
    lines.append("#include <array>")
    lines.append("#include <cstdint>")
    lines.append("#include <limits>")
    lines.append("#include <string_view>")
    lines.append("")
    lines.append("namespace dicom::lookup {")
    lines.append("")
    registry_indices = [str(entry.registry_index) for entry in entries]
    lines.append(f"constexpr std::array<std::uint16_t, {len(entries)}> kKeywordRegistryIndices = {{")
    lines.extend(format_array(registry_indices))
    lines.append("};")
    lines.append("")

    lines.append(f"constexpr std::uint32_t kPerfectHashSeed = {keyword_chd.seed}u;")
    lines.append(f"constexpr std::size_t kPerfectHashTableSize = {keyword_chd.table_size};")
    lines.append(f"constexpr std::size_t kPerfectHashMask = {keyword_chd.mask};")
    lines.append(f"constexpr std::size_t kPerfectHashBucketCount = {keyword_chd.bucket_count};")
    lines.append("")
    slot_values = [
        "std::numeric_limits<std::uint16_t>::max()" if value == SENTINEL else str(value)
        for value in keyword_chd.slots
    ]
    lines.append("constexpr std::array<std::uint16_t, kPerfectHashTableSize> kPerfectHashSlots = {")
    lines.extend(format_array(slot_values))
    lines.append("};")
    lines.append("")
    displacement_values = [str(val) for val in keyword_chd.displacements]
    lines.append("constexpr std::array<std::uint32_t, kPerfectHashBucketCount> kPerfectHashDisplacements = {")
    lines.extend(format_array(displacement_values, per_line=8))
    lines.append("};")
    lines.append("")

    lines.append(f"constexpr std::uint32_t kTagPerfectHashSeed = {tag_chd.seed}u;")
    lines.append(f"constexpr std::size_t kTagPerfectHashTableSize = {tag_chd.table_size};")
    lines.append(f"constexpr std::size_t kTagPerfectHashMask = {tag_chd.mask};")
    lines.append(f"constexpr std::size_t kTagPerfectHashBucketCount = {tag_chd.bucket_count};")
    lines.append("")

    tag_slot_values = [
        "std::numeric_limits<std::uint16_t>::max()" if value == SENTINEL else str(value)
        for value in tag_chd.slots
    ]
    lines.append("constexpr std::array<std::uint16_t, kTagPerfectHashTableSize> kTagPerfectHashSlots = {")
    lines.extend(format_array(tag_slot_values))
    lines.append("};")
    lines.append("")

    tag_displacement_values = [str(val) for val in tag_chd.displacements]
    lines.append(
        "constexpr std::array<std::uint32_t, kTagPerfectHashBucketCount> kTagPerfectHashDisplacements = {"
    )
    lines.extend(format_array(tag_displacement_values, per_line=8))
    lines.append("};")
    lines.append("")

    sorted_wildcards = sorted(wildcard_entries, key=lambda entry: (entry.retired, entry.registry_index))

    lines.append(f"constexpr std::array<std::uint32_t, {len(sorted_wildcards)}> kTagWildcardValues = {{")
    lines.extend(format_array([str(entry.value) for entry in sorted_wildcards]))
    lines.append("};")
    lines.append("")

    lines.append(f"constexpr std::array<std::uint32_t, {len(sorted_wildcards)}> kTagWildcardMasks = {{")
    lines.extend(format_array([str(entry.mask) for entry in sorted_wildcards]))
    lines.append("};")
    lines.append("")

    lines.append(f"constexpr std::array<std::uint16_t, {len(sorted_wildcards)}> kTagWildcardRegistryIndices = {{")
    lines.extend(format_array([str(entry.registry_index) for entry in sorted_wildcards]))
    lines.append("};")
    lines.append("")

    lines.append("}  // namespace dicom::lookup")
    lines.append("")
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--registry", type=Path, default=Path("misc/dictionary/_dataelement_registry.txt"))
    parser.add_argument("--output", type=Path, default=Path("include/dictionary_lookup_tables.hpp"))
    args = parser.parse_args()

    keyword_entries, tag_entries, wildcard_entries = load_entries(args.registry)
    keyword_chd = build_keyword_chd(keyword_entries)
    if not tag_entries:
        raise ValueError("No entries have numeric tag values for tag lookup generation")
    tag_chd = build_tag_chd(tag_entries)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        render(keyword_entries, keyword_chd, tag_entries, tag_chd, wildcard_entries),
        encoding="utf-8",
    )


if __name__ == "__main__":
    main()
