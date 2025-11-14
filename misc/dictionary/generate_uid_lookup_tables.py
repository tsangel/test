#!/usr/bin/env python3
"""Generate include/uid_lookup_tables.hpp using CHD perfect hashes for UID strings."""

from __future__ import annotations

import argparse
import csv
import math
from dataclasses import dataclass
from pathlib import Path
from typing import List, Sequence

SENTINEL = 0xFFFF
GOLDEN_RATIO32 = 0x9E3779B1
HASH_OFFSET64 = 0x6A09E667F3BCC909
HASH_INCR64 = 0x9E3779B97F4A7C15
HASH_MIX1 = 0xBF58476D1CE4E5B9
HASH_MIX2 = 0x94D049BB133111EB
MASK64 = 0xFFFFFFFFFFFFFFFF


@dataclass(frozen=True)
class UidKeyEntry:
    registry_index: int
    text: str


@dataclass(frozen=True)
class ChdTables:
    seed: int
    table_size: int
    mask: int
    bucket_count: int
    slots: List[int]
    displacements: List[int]


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


def keyed_hash64(text: str, seed: int) -> int:
    return mix64(base_hash64(text) ^ ((seed * GOLDEN_RATIO32) & MASK64))


def next_power_of_two(value: int) -> int:
    if value <= 0:
        return 1
    return 1 << (value - 1).bit_length()


def build_chd_from_hashes(
    entries: Sequence[UidKeyEntry],
    hashed_values: Sequence[int],
    load_factor: float = 2.0,
) -> ChdTables:
    if not entries:
        raise ValueError("No entries provided for CHD generation")

    bucket_count = len(entries)
    table_target = max(bucket_count + 1, int(math.ceil(bucket_count * load_factor)))
    table_size = next_power_of_two(table_target)
    mask = table_size - 1

    for seed in range(1, 1 << 16):
        seed_mix = (seed * GOLDEN_RATIO32) & MASK64
        mixed_hashes = [mix64(h ^ seed_mix) for h in hashed_values]
        buckets: List[List[int]] = [[] for _ in range(bucket_count)]
        for idx, mixed in enumerate(mixed_hashes):
            buckets[mixed % bucket_count].append(idx)
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
    raise RuntimeError("Unable to construct CHD tables with the given parameters")


def load_uid_entries(source: Path) -> tuple[List[UidKeyEntry], List[UidKeyEntry]]:
    value_entries: List[UidKeyEntry] = []
    keyword_entries: List[UidKeyEntry] = []
    seen_values: dict[str, int] = {}
    seen_keywords: dict[str, int] = {}
    entry_index = 0
    with source.open("r", encoding="utf-8", newline="") as fh:
        reader = csv.reader(fh, delimiter="\t")
        for idx, cols in enumerate(reader, 1):
            if not cols or all(not col.strip() for col in cols):
                continue
            if cols[0].strip().lower() == "uid_value":
                continue
            if len(cols) != 4:
                raise ValueError(
                    f"expected 4 columns, got {len(cols)} (line {idx})"
                )
            uid_value, _name, keyword, _uid_type = cols
            uid_value = uid_value.strip()
            keyword = keyword.strip()
            if not uid_value:
                raise ValueError(f"missing UID value at line {idx}")
            if uid_value in seen_values:
                raise ValueError(
                    f"duplicate UID value {uid_value!r} at entries {seen_values[uid_value]} and {idx}"
                )
            seen_values[uid_value] = idx
            value_entries.append(UidKeyEntry(entry_index, uid_value))
            if keyword:
                if keyword in seen_keywords:
                    raise ValueError(
                        f"duplicate UID keyword {keyword!r} at indices "
                        f"{seen_keywords[keyword]} and {idx}"
                    )
                seen_keywords[keyword] = idx
                keyword_entries.append(UidKeyEntry(entry_index, keyword))
            entry_index += 1
    return value_entries, keyword_entries


def format_array(values: Sequence[str], indent: str = "    ", per_line: int = 8) -> list[str]:
    lines: list[str] = []
    for i in range(0, len(values), per_line):
        chunk = ", ".join(values[i : i + per_line])
        lines.append(f"{indent}{chunk},")
    return lines


def render(
    value_chd: ChdTables,
    keyword_chd: ChdTables,
    value_slots: Sequence[int],
    keyword_slots: Sequence[int],
) -> str:
    lines: list[str] = []
    lines.append("// Auto-generated UID lookup tables. Do not edit manually.")
    lines.append("#pragma once")
    lines.append("")
    lines.append("#include <array>")
    lines.append("#include <cstdint>")
    lines.append("#include <limits>")
    lines.append("#include <string_view>")
    lines.append("")
    lines.append("namespace dicom::uid_lookup {")
    lines.append("")
    lines.append(f"constexpr std::uint32_t kUidValuePerfectHashSeed = {value_chd.seed}u;")
    lines.append(f"constexpr std::size_t kUidValuePerfectHashTableSize = {value_chd.table_size};")
    lines.append(f"constexpr std::size_t kUidValuePerfectHashMask = {value_chd.mask};")
    lines.append(f"constexpr std::size_t kUidValuePerfectHashBucketCount = {value_chd.bucket_count};")
    lines.append(
        f"constexpr std::array<std::uint16_t, {value_chd.table_size}> kUidValuePerfectHashSlots = {{"
    )
    slot_literals = [f"0x{slot:04X}" for slot in value_slots]
    lines.extend(format_array(slot_literals))
    lines.append("};")
    lines.append("")
    lines.append(
        f"constexpr std::array<std::uint32_t, {value_chd.bucket_count}> kUidValuePerfectHashDisplacements = {{"
    )
    disp_literals = [str(value) + 'u' for value in value_chd.displacements]
    lines.extend(format_array(disp_literals))
    lines.append("};")
    lines.append("")

    lines.append(f"constexpr std::uint32_t kUidKeywordPerfectHashSeed = {keyword_chd.seed}u;")
    lines.append(
        f"constexpr std::size_t kUidKeywordPerfectHashTableSize = {keyword_chd.table_size};"
    )
    lines.append(f"constexpr std::size_t kUidKeywordPerfectHashMask = {keyword_chd.mask};")
    lines.append(f"constexpr std::size_t kUidKeywordPerfectHashBucketCount = {keyword_chd.bucket_count};")
    lines.append(
        f"constexpr std::array<std::uint16_t, {keyword_chd.table_size}> kUidKeywordPerfectHashSlots = {{"
    )
    keyword_slot_literals = [f"0x{slot:04X}" for slot in keyword_slots]
    lines.extend(format_array(keyword_slot_literals))
    lines.append("};")
    lines.append("")
    lines.append(
        f"constexpr std::array<std::uint32_t, {keyword_chd.bucket_count}> kUidKeywordPerfectHashDisplacements = {{"
    )
    keyword_disp_literals = [str(value) + 'u' for value in keyword_chd.displacements]
    lines.extend(format_array(keyword_disp_literals))
    lines.append("};")
    lines.append("")
    lines.append("} // namespace dicom::uid_lookup")
    lines.append("")
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--source",
        type=Path,
        default=Path("misc/dictionary/_uid_registry.tsv"),
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("include/uid_lookup_tables.hpp"),
    )
    parser.add_argument("--load-factor", type=float, default=2.0)
    args = parser.parse_args()

    value_entries, keyword_entries = load_uid_entries(args.source)
    value_hashes = [base_hash64(entry.text) for entry in value_entries]
    keyword_hashes = [base_hash64(entry.text) for entry in keyword_entries]

    value_chd = build_chd_from_hashes(value_entries, value_hashes, args.load_factor)
    keyword_chd = build_chd_from_hashes(keyword_entries, keyword_hashes, args.load_factor)

    header = render(value_chd, keyword_chd, value_chd.slots, keyword_chd.slots)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(header, encoding="utf-8")


if __name__ == "__main__":
    main()
