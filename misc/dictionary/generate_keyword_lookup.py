#!/usr/bin/env python3
"""Generate keyword lookup tables plus CHD/CHM data from the DICOM registry."""

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


@dataclass(frozen=True)
class ChdTables:
    seed: int
    table_size: int
    mask: int
    bucket_count: int
    slots: List[int]
    displacements: List[int]


@dataclass(frozen=True)
class ChmTables:
    seed: int
    vertex_count: int
    vertex_mask: int
    table_size: int
    table_mask: int
    hash_shift: int
    g_values: List[int]
    slots: List[int]


@dataclass(frozen=True)
class BdzTables:
    seed: int
    vertex_count: int
    vertex_mask: int
    table_size: int
    table_mask: int
    hash_shift: int
    hash_double_shift: int
    g_values: List[int]
    slots: List[int]


def should_include(keyword: str, retired: str) -> bool:
    if not keyword:
        return False
    match = RET_YEAR_RE.search(retired)
    if match and int(match.group(1)) < 2010:
        return False
    return True


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


def load_entries(source: Path) -> List[KeywordEntry]:
    rows = parse_rows(source)
    entries: List[KeywordEntry] = []
    for idx, row in enumerate(rows):
        tag, _name, keyword, vr, _vm, retired = row
        keyword = keyword.strip()
        retired = retired.strip()
        if should_include(keyword, retired):
            entries.append(KeywordEntry(idx, keyword, tag, vr))
    return entries

def next_power_of_two(value: int) -> int:
    if value <= 0:
        return 1
    return 1 << (value - 1).bit_length()


def build_chd(entries: Sequence[KeywordEntry], load_factor: float = 2.0) -> ChdTables:
    bucket_count = len(entries)
    if bucket_count == 0:
        raise ValueError("No entries available for CHD table generation")
    table_target = max(bucket_count + 1, int(math.ceil(bucket_count * load_factor)))
    table_size = next_power_of_two(table_target)
    mask = table_size - 1

    hashed_values = [base_hash64(entry.keyword) for entry in entries]

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


def build_chm(entries: Sequence[KeywordEntry]) -> ChmTables:
    count = len(entries)
    if count == 0:
        raise ValueError("No entries available for CHM table generation")
    table_size = next_power_of_two(count)
    table_mask = table_size - 1
    vertex_count = table_size * 2
    vertex_mask = vertex_count - 1
    hash_shift = vertex_mask.bit_length()

    class Edge:
        __slots__ = ("u", "v", "slot", "registry_index")

        def __init__(self, u: int, v: int, slot: int, registry_index: int) -> None:
            self.u = u
            self.v = v
            self.slot = slot
            self.registry_index = registry_index

    def try_seed(seed: int) -> ChmTables | None:
        edges: List[Edge] = []
        adjacency: List[List[int]] = [[] for _ in range(vertex_count)]
        for slot, entry in enumerate(entries):
            hashed = keyword_hash64(entry.keyword, seed)
            u = hashed & vertex_mask
            v = (hashed >> hash_shift) & vertex_mask
            if u == v:
                return None
            edge = Edge(u, v, slot, entry.registry_index)
            idx = len(edges)
            edges.append(edge)
            adjacency[u].append(idx)
            adjacency[v].append(idx)

        degrees = [len(lst) for lst in adjacency]
        queue: List[int] = [idx for idx, deg in enumerate(degrees) if deg == 1]
        used_edges = [False] * len(edges)
        processed = 0

        while queue:
            vertex = queue.pop()
            if degrees[vertex] != 1:
                continue
            for edge_idx in adjacency[vertex]:
                if used_edges[edge_idx]:
                    continue
                used_edges[edge_idx] = True
                processed += 1
                edge = edges[edge_idx]
                other = edge.v if edge.u == vertex else edge.u
                degrees[vertex] -= 1
                degrees[other] -= 1
                if degrees[other] == 1:
                    queue.append(other)
                break

        if processed != len(edges):
            return None

        slots = [SENTINEL] * table_size
        for edge in edges:
            slots[edge.slot] = edge.registry_index

        g_values = [-1] * vertex_count
        visited_edges = [False] * len(edges)
        graph: List[List[tuple[int, int]]] = [[] for _ in range(vertex_count)]
        for idx, edge in enumerate(edges):
            graph[edge.u].append((edge.v, idx))
            graph[edge.v].append((edge.u, idx))

        for vertex in range(vertex_count):
            if g_values[vertex] != -1 or not graph[vertex]:
                if g_values[vertex] == -1:
                    g_values[vertex] = 0
                continue
            g_values[vertex] = 0
            stack = [vertex]
            while stack:
                current = stack.pop()
                for neighbor, edge_idx in graph[current]:
                    if visited_edges[edge_idx]:
                        continue
                    visited_edges[edge_idx] = True
                    slot = edges[edge_idx].slot
                    if g_values[neighbor] == -1:
                        g_values[neighbor] = slot ^ g_values[current]
                        stack.append(neighbor)

        slot_mask = table_mask
        for edge in edges:
            idx = (g_values[edge.u] ^ g_values[edge.v]) & slot_mask
            if idx != edge.slot:
                return None

        return ChmTables(
            seed=seed,
            vertex_count=vertex_count,
            vertex_mask=vertex_mask,
            table_size=table_size,
            table_mask=table_mask,
            hash_shift=hash_shift,
            g_values=g_values,
            slots=slots,
        )

    for seed in range(1, 1 << 16):
        tables = try_seed(seed)
        if tables is not None:
            return tables
    raise RuntimeError("Unable to build CHM tables with the given configuration")


def build_bdz(entries: Sequence[KeywordEntry]) -> BdzTables:
    count = len(entries)
    if count == 0:
        raise ValueError("No entries available for BDZ table generation")

    table_size = next_power_of_two(count)
    table_mask = table_size - 1
    vertex_target = max(count + 1, int(math.ceil(count * 1.3)))
    vertex_count = next_power_of_two(vertex_target)
    vertex_mask = vertex_count - 1
    hash_shift = vertex_mask.bit_length()
    hash_double_shift = hash_shift * 2
    if hash_double_shift >= 64:
        raise RuntimeError("BDZ vertex configuration exceeds hashing capacity")

    class HyperEdge:
        __slots__ = ("vertices", "slot", "registry_index")

        def __init__(self, vertices: tuple[int, int, int], slot: int, registry_index: int) -> None:
            self.vertices = vertices
            self.slot = slot
            self.registry_index = registry_index

    def try_seed(seed: int) -> BdzTables | None:
        edges: List[HyperEdge] = []
        adjacency: List[List[int]] = [[] for _ in range(vertex_count)]
        for slot, entry in enumerate(entries):
            hashed = keyword_hash64(entry.keyword, seed)
            v0 = hashed & vertex_mask
            v1 = (hashed >> hash_shift) & vertex_mask
            v2 = (hashed >> hash_double_shift) & vertex_mask
            if v0 == v1 or v0 == v2 or v1 == v2:
                return None
            index = len(edges)
            edge = HyperEdge((v0, v1, v2), slot, entry.registry_index)
            edges.append(edge)
            adjacency[v0].append(index)
            adjacency[v1].append(index)
            adjacency[v2].append(index)

        degrees = [len(neighbors) for neighbors in adjacency]
        stack: List[int] = [idx for idx, deg in enumerate(degrees) if deg == 1]
        removed_edge_vertex: List[tuple[int, int]] = []
        used_edges = [False] * len(edges)

        while stack:
            vertex = stack.pop()
            if degrees[vertex] == 0:
                continue
            edge_idx = -1
            for candidate in adjacency[vertex]:
                if not used_edges[candidate]:
                    edge_idx = candidate
                    break
            if edge_idx == -1:
                continue
            used_edges[edge_idx] = True
            removed_edge_vertex.append((edge_idx, vertex))
            for neighbor in edges[edge_idx].vertices:
                degrees[neighbor] -= 1
                if degrees[neighbor] == 1:
                    stack.append(neighbor)

        if len(removed_edge_vertex) != len(edges):
            return None

        g_values = [0] * vertex_count
        slots = [SENTINEL] * table_size

        for edge_idx, free_vertex in reversed(removed_edge_vertex):
            edge = edges[edge_idx]
            v0, v1, v2 = edge.vertices
            total = 0
            for vertex in (v0, v1, v2):
                if vertex == free_vertex:
                    continue
                total = (total + g_values[vertex]) & table_mask
            g_values[free_vertex] = (edge.slot - total) & table_mask
            slots[edge.slot] = edge.registry_index

        return BdzTables(
            seed=seed,
            vertex_count=vertex_count,
            vertex_mask=vertex_mask,
            table_size=table_size,
            table_mask=table_mask,
            hash_shift=hash_shift,
            hash_double_shift=hash_double_shift,
            g_values=g_values,
            slots=slots,
        )

    for seed in range(1, 1 << 16):
        tables = try_seed(seed)
        if tables is not None:
            return tables
    raise RuntimeError("Unable to build BDZ tables with the given configuration")


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
    chd: ChdTables,
    chm: ChmTables,
    bdz: BdzTables,
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

    sorted_indices = sorted(entries, key=lambda entry: entry.keyword)
    lines.append(f"constexpr std::array<std::uint16_t, {len(entries)}> kKeywordSortedRegistryIndices = {{")
    lines.extend(format_array([str(entry.registry_index) for entry in sorted_indices]))
    lines.append("};")
    lines.append("")
    lines.append(f"constexpr std::uint32_t kPerfectHashSeed = {chd.seed}u;")
    lines.append(f"constexpr std::size_t kPerfectHashTableSize = {chd.table_size};")
    lines.append(f"constexpr std::size_t kPerfectHashMask = {chd.mask};")
    lines.append(f"constexpr std::size_t kPerfectHashBucketCount = {chd.bucket_count};")
    lines.append("")
    slot_values = [
        "std::numeric_limits<std::uint16_t>::max()" if value == SENTINEL else str(value)
        for value in chd.slots
    ]
    lines.append("constexpr std::array<std::uint16_t, kPerfectHashTableSize> kPerfectHashSlots = {")
    lines.extend(format_array(slot_values))
    lines.append("};")
    lines.append("")
    displacement_values = [str(val) for val in chd.displacements]
    lines.append("constexpr std::array<std::uint32_t, kPerfectHashBucketCount> kPerfectHashDisplacements = {")
    lines.extend(format_array(displacement_values, per_line=8))
    lines.append("};")
    lines.append("")

    lines.append(f"constexpr std::uint32_t kChmSeed = {chm.seed}u;")
    lines.append(f"constexpr std::size_t kChmVertexCount = {chm.vertex_count};")
    lines.append(f"constexpr std::size_t kChmVertexMask = {chm.vertex_mask};")
    lines.append(f"constexpr std::size_t kChmTableSize = {chm.table_size};")
    lines.append(f"constexpr std::size_t kChmTableMask = {chm.table_mask};")
    lines.append(f"constexpr std::uint32_t kChmHashShift = {chm.hash_shift};")
    lines.append("")

    lines.append("constexpr std::array<std::uint32_t, kChmVertexCount> kChmGValues = {")
    lines.extend(format_array([str(value) for value in chm.g_values]))
    lines.append("};")
    lines.append("")

    chm_slot_values = [
        "std::numeric_limits<std::uint16_t>::max()" if value == SENTINEL else str(value)
        for value in chm.slots
    ]
    lines.append("constexpr std::array<std::uint16_t, kChmTableSize> kChmSlots = {")
    lines.extend(format_array(chm_slot_values))
    lines.append("};")
    lines.append("")

    lines.append(f"constexpr std::uint32_t kBdzSeed = {bdz.seed}u;")
    lines.append(f"constexpr std::size_t kBdzVertexCount = {bdz.vertex_count};")
    lines.append(f"constexpr std::size_t kBdzVertexMask = {bdz.vertex_mask};")
    lines.append(f"constexpr std::size_t kBdzTableSize = {bdz.table_size};")
    lines.append(f"constexpr std::size_t kBdzTableMask = {bdz.table_mask};")
    lines.append(f"constexpr std::uint32_t kBdzHashShift = {bdz.hash_shift};")
    lines.append(f"constexpr std::uint32_t kBdzHashDoubleShift = {bdz.hash_double_shift};")
    lines.append("")

    lines.append("constexpr std::array<std::uint32_t, kBdzVertexCount> kBdzGValues = {")
    lines.extend(format_array([str(value) for value in bdz.g_values]))
    lines.append("};")
    lines.append("")

    bdz_slot_values = [
        "std::numeric_limits<std::uint16_t>::max()" if value == SENTINEL else str(value)
        for value in bdz.slots
    ]
    lines.append("constexpr std::array<std::uint16_t, kBdzTableSize> kBdzSlots = {")
    lines.extend(format_array(bdz_slot_values))
    lines.append("};")
    lines.append("")

    lines.append("}  // namespace dicom::lookup")
    lines.append("")
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--registry", type=Path, default=Path("misc/dictionary/_dataelement_registry.txt"))
    parser.add_argument("--output", type=Path, default=Path("include/keyword_lookup_tables.hpp"))
    args = parser.parse_args()

    entries = load_entries(args.registry)
    chd = build_chd(entries)
    chm = build_chm(entries)
    bdz = build_bdz(entries)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(render(entries, chd, chm, bdz), encoding="utf-8")


if __name__ == "__main__":
    main()
