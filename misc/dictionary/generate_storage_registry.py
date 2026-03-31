#!/usr/bin/env python3
"""Generate include/storage/storage_registry.hpp and src/storage/storage_registry.cpp."""

from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass
from pathlib import Path

from _generator_common import write_text_if_changed


@dataclass(frozen=True)
class SopClassIodMapRow:
    uid_index: int
    uid_value: str
    iod_xml_id: str
    iod_title: str
    part04_section_id: str
    retired: bool


@dataclass(frozen=True)
class IodComponentRow:
    iod_xml_id: str
    entry_index: int
    entry_kind: str
    ie_name: str
    component_name: str
    component_section_id: str
    usage: str
    usage_condition_text: str
    fg_scope: str
    source_table_xml_id: str


@dataclass(frozen=True)
class ComponentAttributeRuleRow:
    component_section_id: str
    component_kind: str
    row_index: int
    path_signature: str
    nesting_level: int
    tag_value: int
    declared_type: str
    condition_text: str
    may_be_present_otherwise: bool
    origin_section_id: str
    origin_table_xml_id: str


@dataclass(frozen=True)
class IodAttributeOverrideRow:
    iod_xml_id: str
    path_signature: str
    tag_text: str
    tag_value: int
    effective_type_override: str
    override_condition_text: str
    source_section_id: str
    reason: str


@dataclass(frozen=True)
class KeyRange:
    key: str
    begin: int
    end: int


@dataclass(frozen=True)
class StoragePathNodeRow:
    incoming_tag: int
    parent_node_index: int
    first_edge_index: int
    edge_count: int
    first_rule_index: int
    rule_count: int


@dataclass(frozen=True)
class StoragePathEdgeRow:
    tag_value: int
    next_node_index: int


@dataclass(frozen=True)
class StorageContextRow:
    component_section_id: str
    context_key: str
    context_name: str
    context_kind: str
    is_root: bool
    is_recursive: bool


@dataclass(frozen=True)
class StorageContextTransitionRow:
    component_section_id: str
    from_context_key: str
    sequence_tag_value: int
    next_context_key: str
    transition_condition_text: str
    push_path: bool
    is_recursive: bool


@dataclass(frozen=True)
class StorageContextRuleIndexRow:
    component_section_id: str
    context_key: str
    rule_row_index: int


@dataclass(frozen=True)
class StorageConditionProgramRow:
    op: str
    tag_value: int
    arg0: int
    arg1: int
    source_text: str
    value_index: int = 0


HEADER_PREAMBLE = """// Auto-generated from:
//   - {uid_source}
//   - {sop_source}
//   - {component_source}
//   - {rule_source}
//   - {override_source}
//   - {context_source}
//   - {context_transition_source}
//   - {context_rule_index_source}
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace dicom::storage {{

enum class ComponentKind : std::uint8_t {{
    Unknown = 0,
    Module,
    FunctionalGroup,
}};

enum class ModuleUsage : std::uint8_t {{
    Unknown = 0,
    Mandatory,
    Conditional,
    UserOption,
}};

enum class TypeDesignation : std::uint8_t {{
    Unknown = 0,
    Type1,
    Type1C,
    Type2,
    Type2C,
    Type3,
}};

enum class StorageContextKind : std::uint8_t {{
    Unknown = 0,
    Table,
}};

enum class StorageConditionOp : std::uint8_t {{
    Unknown = 0,
    External,
    Present,
    NotPresent,
    Empty,
    EqText,
    NeText,
    ValueEqText,
    ValueNeText,
    GreaterThan,
    AnyPresent,
    AllAbsent,
    TagEqAnyTag,
    TagNeAnyTag,
    And,
    Or,
    WaveformFilterKindEq,
}};

struct SopClassStorageMapEntry {{
    std::uint16_t uid_index;
    std::string_view iod_xml_id;
    std::string_view iod_title;
    std::string_view part04_section_id;
    bool retired;
}};

struct StorageComponentRegistryEntry {{
    std::uint16_t ie_name_id;
    std::uint16_t component_name_id;
    std::uint16_t component_section_id_id;
    std::uint16_t usage_condition_program_index;
    ComponentKind entry_kind;
    ModuleUsage usage;

    [[nodiscard]] std::string_view ie_name() const noexcept;
    [[nodiscard]] std::string_view component_name() const noexcept;
    [[nodiscard]] std::string_view component_section_id() const noexcept;
    [[nodiscard]] std::string_view usage_condition_text() const noexcept;
}};

struct StoragePathNodeEntry {{
    std::uint32_t incoming_tag;
    std::uint32_t first_rule_index;
    std::uint16_t first_edge_index;
    std::uint16_t edge_count;
    std::uint16_t rule_count;
    std::uint16_t parent_node_index;
}};

struct StoragePathEdgeEntry {{
    std::uint32_t tag_value;
    std::uint16_t next_node_index;
}};

struct StorageContextEntry {{
    std::uint16_t component_section_id_id;
    std::uint16_t context_key_id;
    std::uint16_t context_name_id;
    std::uint32_t first_transition_index;
    std::uint32_t first_rule_index;
    std::uint16_t transition_count;
    std::uint16_t rule_count;
    StorageContextKind context_kind;
    bool is_root;
    bool is_recursive;

    [[nodiscard]] std::string_view component_section_id() const noexcept;
    [[nodiscard]] std::string_view context_key() const noexcept;
    [[nodiscard]] std::string_view context_name() const noexcept;
}};

struct StorageContextTransitionEntry {{
    std::uint16_t next_context_index;
    std::uint16_t transition_condition_text_id;
    std::uint32_t sequence_tag_value;
    bool push_path;
    bool is_recursive;

    [[nodiscard]] std::string_view transition_condition_text() const noexcept;
}};

struct StorageConditionProgramEntry {{
    std::uint32_t arg0;
    std::uint32_t arg1;
    std::uint32_t tag_value;
    std::uint16_t source_text_id;
    StorageConditionOp op;
    std::uint8_t value_index;

    [[nodiscard]] std::string_view source_text() const noexcept;
}};

struct ComponentAttributeRuleEntry {{
    std::uint16_t component_section_id_id;
    std::uint16_t path_node_index;
    std::uint32_t tag_value;
    TypeDesignation declared_type;
    std::uint16_t condition_program_index;
    bool may_be_present_otherwise;

    [[nodiscard]] std::string_view component_section_id() const noexcept;
    [[nodiscard]] std::string_view condition_text() const noexcept;
}};

struct StorageAttributeOverrideEntry {{
    std::string_view iod_xml_id;
    std::uint16_t path_node_index;
    std::uint32_t tag_value;
    std::string_view tag_text;
    TypeDesignation effective_type_override;
    std::string_view override_condition_text;
    std::string_view source_section_id;
    std::string_view reason;
}};

struct KeyRangeEntry {{
    std::string_view key;
    std::uint32_t begin;
    std::uint32_t end;
}};

inline constexpr std::uint16_t kInvalidSopClassStorageMapIndex =
    std::numeric_limits<std::uint16_t>::max();
inline constexpr std::uint16_t kInvalidStorageComponentStringId =
    std::numeric_limits<std::uint16_t>::max();
inline constexpr std::uint16_t kInvalidStoragePathNodeIndex =
    std::numeric_limits<std::uint16_t>::max();
inline constexpr std::uint16_t kInvalidStorageContextIndex =
    std::numeric_limits<std::uint16_t>::max();
inline constexpr std::uint16_t kInvalidStorageConditionStringId =
    std::numeric_limits<std::uint16_t>::max();
inline constexpr std::uint16_t kInvalidStorageConditionProgramIndex =
    std::numeric_limits<std::uint16_t>::max();

"""

HEADER_POSTAMBLE = """
[[nodiscard]] inline std::string_view storage_component_string(
    std::uint16_t string_id) noexcept {
    return string_id < kStorageComponentStringTable.size()
               ? kStorageComponentStringTable[string_id]
               : std::string_view{};
}

[[nodiscard]] inline std::string_view storage_condition_string(
    std::uint16_t string_id) noexcept {
    return string_id < kStorageConditionStringTable.size()
               ? kStorageConditionStringTable[string_id]
               : std::string_view{};
}

[[nodiscard]] inline const StorageConditionProgramEntry* find_storage_condition_program_entry(
    std::uint16_t program_index) noexcept {
    return program_index < kStorageConditionProgramRegistry.size()
               ? &kStorageConditionProgramRegistry[program_index]
               : nullptr;
}

[[nodiscard]] inline std::string_view StorageComponentRegistryEntry::ie_name() const noexcept {
    return storage_component_string(ie_name_id);
}

[[nodiscard]] inline std::string_view StorageComponentRegistryEntry::component_name() const noexcept {
    return storage_component_string(component_name_id);
}

[[nodiscard]] inline std::string_view StorageComponentRegistryEntry::component_section_id() const noexcept {
    return storage_component_string(component_section_id_id);
}

[[nodiscard]] inline std::string_view StorageComponentRegistryEntry::usage_condition_text() const noexcept {
    if (const auto* program = find_storage_condition_program_entry(usage_condition_program_index)) {
        return program->source_text();
    }
    return std::string_view{};
}

[[nodiscard]] inline std::string_view StorageContextEntry::component_section_id() const noexcept {
    return storage_component_string(component_section_id_id);
}

[[nodiscard]] inline std::string_view StorageContextEntry::context_key() const noexcept {
    return storage_component_string(context_key_id);
}

[[nodiscard]] inline std::string_view StorageContextEntry::context_name() const noexcept {
    return storage_component_string(context_name_id);
}

[[nodiscard]] inline std::string_view StorageContextTransitionEntry::transition_condition_text() const noexcept {
    return storage_component_string(transition_condition_text_id);
}

[[nodiscard]] inline std::string_view StorageConditionProgramEntry::source_text() const noexcept {
    return storage_condition_string(source_text_id);
}

[[nodiscard]] inline std::string_view ComponentAttributeRuleEntry::component_section_id() const noexcept {
    return storage_component_string(component_section_id_id);
}

[[nodiscard]] inline std::string_view ComponentAttributeRuleEntry::condition_text() const noexcept {
    if (const auto* program = find_storage_condition_program_entry(condition_program_index)) {
        return program->source_text();
    }
    return std::string_view{};
}

"""

SOURCE_PREAMBLE = """// Auto-generated from:
//   - {uid_source}
//   - {sop_source}
//   - {component_source}
//   - {rule_source}
//   - {override_source}
//   - {context_source}
//   - {context_transition_source}
//   - {context_rule_index_source}
#include "{header_include}"

namespace dicom::storage {{

"""

EXTERNAL_CONDITION_COLUMNS = (
    "source_kind",
    "component_section_id",
    "component_name",
    "ie_name",
    "component_kind",
    "usage_or_type",
    "path_signature",
    "tag_text",
    "tag_value",
    "condition_program_index",
    "condition_text",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--sopclass-iod-source",
        type=Path,
        default=Path("misc/dictionary/_sopclass_iod_map.tsv"),
    )
    parser.add_argument(
        "--uid-registry-source",
        type=Path,
        default=Path("misc/dictionary/_uid_registry.tsv"),
    )
    parser.add_argument(
        "--iod-component-source",
        type=Path,
        default=Path("misc/dictionary/_iod_component_registry.tsv"),
    )
    parser.add_argument(
        "--component-rule-source",
        type=Path,
        default=Path("misc/dictionary/_component_attribute_rules.tsv"),
    )
    parser.add_argument(
        "--override-source",
        type=Path,
        default=Path("misc/dictionary/_iod_attribute_overrides.tsv"),
    )
    parser.add_argument(
        "--context-source",
        type=Path,
        default=Path("misc/dictionary/_storage_context_registry.tsv"),
    )
    parser.add_argument(
        "--context-transition-source",
        type=Path,
        default=Path("misc/dictionary/_storage_context_transition_registry.tsv"),
    )
    parser.add_argument(
        "--context-rule-index-source",
        type=Path,
        default=Path("misc/dictionary/_storage_context_rule_index_registry.tsv"),
    )
    parser.add_argument(
        "--header-output",
        type=Path,
        default=Path("include/storage/storage_registry.hpp"),
    )
    parser.add_argument(
        "--source-output",
        type=Path,
        default=Path("src/storage/storage_registry.cpp"),
    )
    parser.add_argument(
        "--external-condition-output",
        type=Path,
        default=Path("misc/dictionary/_storage_external_conditions.tsv"),
    )
    parser.add_argument(
        "--output",
        dest="header_output",
        type=Path,
        help=argparse.SUPPRESS,
    )
    return parser.parse_args()


def escape(value: str) -> str:
    return value.replace("\\", r"\\").replace('"', r"\"")


def parse_bool(value: str) -> bool:
    return value.strip().upper() in {"Y", "YES", "TRUE", "1"}


def parse_int(value: str) -> int:
    return int(value.strip()) if value.strip() else 0


def parse_tag_value(tag_text: str) -> int:
    cleaned = tag_text.replace("(", "").replace(")", "").replace(",", "").replace(" ", "")
    if len(cleaned) != 8:
        return 0
    try:
        return int(cleaned, 16)
    except ValueError:
        return 0


def format_tag_text(tag_value: int) -> str:
    if tag_value <= 0:
        return ""
    return f"({(tag_value >> 16) & 0xFFFF:04X},{tag_value & 0xFFFF:04X})"


def extract_required_if_clause(text: str) -> str:
    for prefix in (
        "Required if ",
        "required if ",
        "shall be present if ",
        "Shall be present if ",
    ):
        pos = text.find(prefix)
        if pos == -1:
            continue
        clause = text[pos + len(prefix) :]
        end = len(clause)
        dot = clause.find(". ")
        semi = clause.find(";")
        if dot != -1:
            end = min(end, dot)
        if semi != -1:
            end = min(end, semi)
        clause = clause[:end].strip(" \t\r\n,")
        clause = " ".join(clause.split())
        if clause.endswith("."):
            clause = clause[:-1].rstrip()
        if clause:
            return clause
    return ""


def normalize_condition_text(clause: str) -> str:
    low = clause.lower()
    if low == "the filter is a high-pass filter":
        return "@waveform_filter_kind = HIGH_PASS"
    if low == "the filter is a low-pass filter":
        return "@waveform_filter_kind = LOW_PASS"
    if low == "the filter is a notch filter":
        return "@waveform_filter_kind = NOTCH"
    return clause


def trim_ascii(text: str) -> str:
    return text.strip(" \t\r\n,")


def strip_balanced_quotes(text: str) -> str:
    text = trim_ascii(text)
    while len(text) >= 2 and (
        (text[0] == '"' and text[-1] == '"') or (text[0] == "'" and text[-1] == "'")
    ):
        text = trim_ascii(text[1:-1])
    return text


def extract_tags_from_text(text: str) -> tuple[int, ...]:
    tags: list[int] = []
    for index in range(0, max(0, len(text) - 10)):
        if text[index] != "(" or text[index + 5] != "," or text[index + 10] != ")":
            continue
        candidate = text[index + 1 : index + 10]
        hex_text = candidate[:4] + candidate[5:]
        if len(hex_text) != 8 or any(ch not in "0123456789abcdefABCDEF" for ch in hex_text):
            continue
        tags.append(int(hex_text, 16))
    return tuple(tags)


def split_expected_values_text(text: str) -> tuple[str, ...]:
    values: list[str] = []
    start = 0
    while start < len(text):
        end = text.find(",", start)
        chunk = strip_balanced_quotes(text[start:] if end == -1 else text[start:end])
        if chunk:
            low_chunk = chunk.lower()
            if low_chunk.startswith("or "):
                values.append(strip_balanced_quotes(chunk[3:]))
            else:
                values.append(chunk)
        if end == -1:
            break
        start = end + 1

    expanded: list[str] = []
    for chunk in values:
        low_chunk = chunk.lower()
        or_pos = low_chunk.find(" or ")
        if or_pos == -1:
            expanded.append(strip_balanced_quotes(chunk))
            continue
        expanded.append(strip_balanced_quotes(chunk[:or_pos]))
        expanded.append(strip_balanced_quotes(chunk[or_pos + 4 :]))
    return tuple(value for value in expanded if value)


def canonicalize_condition_literal(value: str) -> str:
    return trim_ascii(strip_balanced_quotes(value)).lower()


def infer_subject_tag_from_clause(
    clause: str, implied_subject_tag: int | None = None
) -> int | None:
    tags = extract_tags_from_text(clause)
    if len(tags) == 1:
        return tags[0]
    if tags or implied_subject_tag is None:
        return None
    if trim_ascii(clause).lower().startswith("value "):
        return implied_subject_tag
    return None


class ConditionCompiler:
    def __init__(self) -> None:
        self._string_pool: list[str] = [""]
        self._string_indices: dict[str, int] = {"": 0}
        self._string_ref_pool: list[int] = []
        self._string_ref_ranges: dict[tuple[str, ...], tuple[int, int]] = {}
        self._tag_ref_pool: list[int] = []
        self._tag_ref_ranges: dict[tuple[int, ...], tuple[int, int]] = {}
        self._programs: list[StorageConditionProgramRow] = []
        self._program_indices: dict[tuple[object, ...], int] = {}

    def strings(self) -> list[str]:
        return self._string_pool

    def string_ref_pool(self) -> list[int]:
        return self._string_ref_pool

    def tag_ref_pool(self) -> list[int]:
        return self._tag_ref_pool

    def programs(self) -> list[StorageConditionProgramRow]:
        return self._programs

    def compile_clause(self, clause: str) -> int:
        clause = normalize_condition_text(" ".join(clause.split()))
        if not clause:
            return 0xFFFF
        return self._compile_required_clause(clause, None)

    def _intern_string(self, value: str) -> int:
        if value in self._string_indices:
            return self._string_indices[value]
        index = len(self._string_pool)
        if index >= 0xFFFF:
            raise RuntimeError("Storage condition string pool exceeded uint16_t capacity")
        self._string_indices[value] = index
        self._string_pool.append(value)
        return index

    def _intern_string_refs(self, values: tuple[str, ...]) -> tuple[int, int]:
        canonical_values = tuple(canonicalize_condition_literal(value) for value in values if value)
        if canonical_values in self._string_ref_ranges:
            return self._string_ref_ranges[canonical_values]
        begin = len(self._string_ref_pool)
        for value in canonical_values:
            self._string_ref_pool.append(self._intern_string(value))
        result = (begin, len(canonical_values))
        self._string_ref_ranges[canonical_values] = result
        return result

    def _intern_tag_refs(self, values: tuple[int, ...]) -> tuple[int, int]:
        if values in self._tag_ref_ranges:
            return self._tag_ref_ranges[values]
        begin = len(self._tag_ref_pool)
        self._tag_ref_pool.extend(values)
        result = (begin, len(values))
        self._tag_ref_ranges[values] = result
        return result

    def _intern_program(
        self,
        op: str,
        *,
        tag_value: int = 0,
        arg0: int = 0,
        arg1: int = 0,
        source_text: str = "",
        value_index: int = 0,
    ) -> int:
        if source_text:
            self._intern_string(source_text)
        key = (op, tag_value, arg0, arg1, source_text, value_index)
        if key in self._program_indices:
            return self._program_indices[key]
        index = len(self._programs)
        if index >= 0xFFFF:
            raise RuntimeError("Storage condition program pool exceeded uint16_t capacity")
        self._program_indices[key] = index
        self._programs.append(
            StorageConditionProgramRow(
                op=op,
                tag_value=tag_value,
                arg0=arg0,
                arg1=arg1,
                source_text=source_text,
                value_index=value_index,
            )
        )
        return index

    def _compile_required_clause(
        self, clause: str, implied_subject_tag: int | None
    ) -> int:
        clause = trim_ascii(clause)
        low = clause.lower()
        tags = extract_tags_from_text(clause)
        if not tags and infer_subject_tag_from_clause(clause, implied_subject_tag) is None:
            return self._intern_program("external", source_text=clause)

        if low.startswith("either ") and " or " in low and " is present" in low and tags:
            begin, count = self._intern_tag_refs(tags)
            return self._intern_program(
                "any_present", arg0=begin, arg1=count, source_text=clause
            )

        if len(tags) >= 2 and " are not present" in low:
            begin, count = self._intern_tag_refs(tags)
            return self._intern_program(
                "all_absent", arg0=begin, arg1=count, source_text=clause
            )

        for connector in (", and if ", " and if ", ", and ", " and "):
            pos = low.find(connector)
            if pos == -1:
                continue
            lhs = trim_ascii(clause[:pos])
            rhs = trim_ascii(clause[pos + len(connector) :])
            if rhs.lower().startswith("if "):
                rhs = trim_ascii(rhs[3:])
            rhs_implied_subject = None
            if not extract_tags_from_text(rhs):
                rhs_implied_subject = infer_subject_tag_from_clause(lhs, implied_subject_tag)
            lhs_program = self._compile_required_clause(lhs, implied_subject_tag)
            rhs_program = self._compile_required_clause(rhs, rhs_implied_subject)
            if lhs_program == 0xFFFF or rhs_program == 0xFFFF:
                return self._intern_program("external", source_text=clause)
            return self._intern_program(
                "and", arg0=lhs_program, arg1=rhs_program, source_text=clause
            )

        return self._compile_simple_clause(clause, implied_subject_tag)

    def _compile_simple_clause(
        self, clause: str, implied_subject_tag: int | None
    ) -> int:
        clause = trim_ascii(clause)
        low = clause.lower()
        if low.startswith("@waveform_filter_kind = "):
            expected = trim_ascii(clause[len("@waveform_filter_kind = ") :])
            begin, count = self._intern_string_refs((expected,))
            return self._intern_program(
                "waveform_filter_kind_eq",
                arg0=begin,
                arg1=count,
                source_text=clause,
            )

        tags = list(extract_tags_from_text(clause))
        if not tags:
            inferred = infer_subject_tag_from_clause(clause, implied_subject_tag)
            if inferred is not None:
                tags.append(inferred)

        if len(tags) != 1:
            if len(tags) >= 2 and " are present" in low and " or " in low:
                begin, count = self._intern_tag_refs(tuple(tags))
                return self._intern_program(
                    "any_present", arg0=begin, arg1=count, source_text=clause
                )
            if len(tags) >= 2 and " is not " in low:
                begin, count = self._intern_tag_refs(tuple(tags[1:]))
                return self._intern_program(
                    "tag_ne_any_tag",
                    tag_value=tags[0],
                    arg0=begin,
                    arg1=count,
                    source_text=clause,
                )
            if len(tags) >= 2 and " is " in low:
                begin, count = self._intern_tag_refs(tuple(tags[1:]))
                return self._intern_program(
                    "tag_eq_any_tag",
                    tag_value=tags[0],
                    arg0=begin,
                    arg1=count,
                    source_text=clause,
                )
            return self._intern_program("external", source_text=clause)

        tag = tags[0]
        if " value " in low:
            value_pos = low.find(" value ")
            is_not_pos = low.find(" is not ", value_pos)
            is_pos = low.find(" is ", value_pos)
            if is_not_pos != -1 and (is_pos == -1 or is_not_pos < is_pos):
                index_text = trim_ascii(
                    clause[value_pos + len(" value ") : is_not_pos]
                )
                try:
                    value_index = int(index_text)
                except ValueError:
                    return self._intern_program("external", source_text=clause)
                begin, count = self._intern_string_refs(
                    split_expected_values_text(clause[is_not_pos + len(" is not ") :])
                )
                return self._intern_program(
                    "value_ne_text",
                    tag_value=tag,
                    arg0=begin,
                    arg1=count,
                    source_text=clause,
                    value_index=value_index,
                )
            if is_pos != -1:
                index_text = trim_ascii(clause[value_pos + len(" value ") : is_pos])
                try:
                    value_index = int(index_text)
                except ValueError:
                    return self._intern_program("external", source_text=clause)
                begin, count = self._intern_string_refs(
                    split_expected_values_text(clause[is_pos + len(" is ") :])
                )
                return self._intern_program(
                    "value_eq_text",
                    tag_value=tag,
                    arg0=begin,
                    arg1=count,
                    source_text=clause,
                    value_index=value_index,
                )

        if " is not present" in low or " is absent" in low:
            return self._intern_program("not_present", tag_value=tag, source_text=clause)
        if " is empty" in low:
            return self._intern_program("empty", tag_value=tag, source_text=clause)
        if " is greater than " in low:
            pos = low.find(" is greater than ")
            threshold_text = trim_ascii(clause[pos + len(" is greater than ") :])
            try:
                threshold = int(threshold_text)
            except ValueError:
                return self._intern_program("external", source_text=clause)
            return self._intern_program(
                "greater_than", tag_value=tag, arg0=threshold, source_text=clause
            )
        for needle in (
            " is present and equals ",
            " is present and has a value of ",
            " is present and has the value ",
            " has value ",
            " has the value ",
            " has a value of ",
            " = ",
            " equals ",
            " is ",
        ):
            pos = low.find(needle)
            if pos == -1:
                continue
            if needle == " is ":
                expected = strip_balanced_quotes(clause[pos + len(needle) :])
                if expected in ("present", "not present", "empty"):
                    continue
            begin, count = self._intern_string_refs(
                split_expected_values_text(clause[pos + len(needle) :])
            )
            return self._intern_program(
                "eq_text", tag_value=tag, arg0=begin, arg1=count, source_text=clause
            )
        if " is present" in low:
            return self._intern_program("present", tag_value=tag, source_text=clause)
        if " is not " in low:
            pos = low.find(" is not ")
            expected = strip_balanced_quotes(clause[pos + len(" is not ") :])
            if expected and expected not in ("present", "empty"):
                begin, count = self._intern_string_refs(split_expected_values_text(expected))
                return self._intern_program(
                    "ne_text", tag_value=tag, arg0=begin, arg1=count, source_text=clause
                )

        return self._intern_program("external", source_text=clause)


def parse_path_signature_tags(path_signature: str) -> tuple[int, ...]:
    text = path_signature.strip()
    if not text:
        return ()
    values: list[int] = []
    for token in text.split("/"):
        cleaned = token.strip()
        if len(cleaned) != 8:
            raise RuntimeError(f"Invalid path token {cleaned!r} in path {path_signature!r}")
        try:
            values.append(int(cleaned, 16))
        except ValueError as exc:
            raise RuntimeError(
                f"Invalid hexadecimal path token {cleaned!r} in path {path_signature!r}"
            ) from exc
    return tuple(values)


def load_uid_indices(path: Path) -> tuple[dict[str, int], int]:
    indices: dict[str, int] = {}
    count = 0
    with path.open("r", encoding="utf-8", newline="") as fh:
        reader = csv.DictReader(fh, delimiter="\t")
        for index, record in enumerate(reader):
            uid_value = record["uid_value"].strip()
            if uid_value:
                indices[uid_value] = index
            count = index + 1
    return indices, count


def load_sopclass_iod_rows(path: Path, uid_indices: dict[str, int]) -> list[SopClassIodMapRow]:
    rows: list[SopClassIodMapRow] = []
    with path.open("r", encoding="utf-8", newline="") as fh:
        reader = csv.DictReader(fh, delimiter="\t")
        for record in reader:
            uid_value = record["uid_value"].strip()
            uid_index = uid_indices.get(uid_value)
            if uid_index is None:
                raise RuntimeError(f"UID {uid_value!r} from {path} not found in UID registry")
            rows.append(
                SopClassIodMapRow(
                    uid_index=uid_index,
                    uid_value=uid_value,
                    iod_xml_id=record["iod_xml_id"].strip(),
                    iod_title=record["iod_title"].strip(),
                    part04_section_id=record["part04_section_id"].strip(),
                    retired=parse_bool(record["retired"]),
                )
            )
    rows.sort(key=lambda row: (row.uid_index, row.uid_value))
    return rows


def load_iod_component_rows(path: Path) -> list[IodComponentRow]:
    rows: list[IodComponentRow] = []
    with path.open("r", encoding="utf-8", newline="") as fh:
        reader = csv.DictReader(fh, delimiter="\t")
        for record in reader:
            rows.append(
                IodComponentRow(
                    iod_xml_id=record["iod_xml_id"].strip(),
                    entry_index=parse_int(record["entry_index"]),
                    entry_kind=record["entry_kind"].strip(),
                    ie_name=record["ie_name"].strip(),
                    component_name=record["component_name"].strip(),
                    component_section_id=record["component_section_id"].strip(),
                    usage=record["usage"].strip(),
                    usage_condition_text=record["usage_condition_text"].strip(),
                    fg_scope=record["fg_scope"].strip(),
                    source_table_xml_id=record["source_table_xml_id"].strip(),
                )
            )
    rows.sort(key=lambda row: (row.iod_xml_id, row.entry_index, row.component_section_id))
    return rows


def load_component_rule_rows(path: Path) -> list[ComponentAttributeRuleRow]:
    rows: list[ComponentAttributeRuleRow] = []
    with path.open("r", encoding="utf-8", newline="") as fh:
        reader = csv.DictReader(fh, delimiter="\t")
        for record in reader:
            tag_text = record["tag"].strip()
            description_text = record["description_text"].strip()
            rows.append(
                ComponentAttributeRuleRow(
                    component_section_id=record["component_section_id"].strip(),
                    component_kind=record["component_kind"].strip(),
                    row_index=parse_int(record["row_index"]),
                    path_signature=record["path_signature"].strip(),
                    nesting_level=parse_int(record["nesting_level"]),
                    tag_value=parse_tag_value(tag_text),
                    declared_type=record["type"].strip(),
                    condition_text=normalize_condition_text(
                        extract_required_if_clause(description_text)
                    ),
                    may_be_present_otherwise=parse_bool(record["may_be_present_otherwise"]),
                    origin_section_id=record["origin_section_id"].strip(),
                    origin_table_xml_id=record["origin_table_xml_id"].strip(),
                )
            )
    rows.sort(
        key=lambda row: (
            row.component_section_id,
            row.component_kind,
            row.row_index,
            row.path_signature,
        )
    )
    return rows


def load_override_rows(path: Path) -> list[IodAttributeOverrideRow]:
    if not path.exists():
        return []
    rows: list[IodAttributeOverrideRow] = []
    with path.open("r", encoding="utf-8", newline="") as fh:
        reader = csv.DictReader(fh, delimiter="\t")
        for record in reader:
            tag_text = record["tag"].strip()
            rows.append(
                IodAttributeOverrideRow(
                    iod_xml_id=record["iod_xml_id"].strip(),
                    path_signature=record["path_signature"].strip(),
                    tag_text=tag_text,
                    tag_value=parse_tag_value(tag_text),
                    effective_type_override=record["effective_type_override"].strip(),
                    override_condition_text=record["override_condition_text"].strip(),
                    source_section_id=record["source_section_id"].strip(),
                    reason=record["reason"].strip(),
                )
            )
    rows.sort(key=lambda row: (row.iod_xml_id, row.path_signature, row.tag_text))
    return rows


def load_storage_context_rows(path: Path) -> list[StorageContextRow]:
    if not path.exists():
        return []
    rows: list[StorageContextRow] = []
    with path.open("r", encoding="utf-8", newline="") as fh:
        reader = csv.DictReader(fh, delimiter="\t")
        for record in reader:
            rows.append(
                StorageContextRow(
                    component_section_id=record["component_section_id"].strip(),
                    context_key=record["context_key"].strip(),
                    context_name=record["context_name"].strip(),
                    context_kind=record["context_kind"].strip(),
                    is_root=parse_bool(record["is_root"]),
                    is_recursive=parse_bool(record["is_recursive"]),
                )
            )
    rows.sort(key=lambda row: (row.component_section_id, not row.is_root, row.context_key))
    return rows


def load_storage_context_transition_rows(path: Path) -> list[StorageContextTransitionRow]:
    if not path.exists():
        return []
    rows: list[StorageContextTransitionRow] = []
    with path.open("r", encoding="utf-8", newline="") as fh:
        reader = csv.DictReader(fh, delimiter="\t")
        for record in reader:
            rows.append(
                StorageContextTransitionRow(
                    component_section_id=record["component_section_id"].strip(),
                    from_context_key=record["from_context_key"].strip(),
                    sequence_tag_value=parse_tag_value(record["sequence_tag"].strip()),
                    next_context_key=record["next_context_key"].strip(),
                    transition_condition_text=record["transition_condition_text"].strip(),
                    push_path=parse_bool(record["push_path"]),
                    is_recursive=parse_bool(record["is_recursive"]),
                )
            )
    rows.sort(
        key=lambda row: (
            row.component_section_id,
            row.from_context_key,
            row.sequence_tag_value,
            row.next_context_key,
        )
    )
    return rows


def load_storage_context_rule_index_rows(path: Path) -> list[StorageContextRuleIndexRow]:
    if not path.exists():
        return []
    rows: list[StorageContextRuleIndexRow] = []
    with path.open("r", encoding="utf-8", newline="") as fh:
        reader = csv.DictReader(fh, delimiter="\t")
        for record in reader:
            rows.append(
                StorageContextRuleIndexRow(
                    component_section_id=record["component_section_id"].strip(),
                    context_key=record["context_key"].strip(),
                    rule_row_index=parse_int(record["rule_row_index"]),
                )
            )
    rows.sort(key=lambda row: (row.component_section_id, row.context_key, row.rule_row_index))
    return rows


def build_path_graph(
    rule_rows: list[ComponentAttributeRuleRow],
    override_rows: list[IodAttributeOverrideRow],
) -> tuple[
    list[StoragePathNodeRow],
    list[StoragePathEdgeRow],
    list[int],
    dict[str, int],
]:
    child_maps: list[dict[int, int]] = [{}]
    parent_indices = [0xFFFF]
    incoming_tags = [0]
    terminal_rules: list[list[int]] = [[]]
    path_node_indices: dict[str, int] = {}

    def intern_path(path_signature: str) -> int:
        if path_signature in path_node_indices:
            return path_node_indices[path_signature]
        tags = parse_path_signature_tags(path_signature)
        node_index = 0
        for tag_value in tags:
            next_node_index = child_maps[node_index].get(tag_value)
            if next_node_index is None:
                next_node_index = len(child_maps)
                if next_node_index >= 0xFFFF:
                    raise RuntimeError("Storage path graph exceeded uint16_t node capacity")
                child_maps[node_index][tag_value] = next_node_index
                child_maps.append({})
                parent_indices.append(node_index)
                incoming_tags.append(tag_value)
                terminal_rules.append([])
            node_index = next_node_index
        path_node_indices[path_signature] = node_index
        return node_index

    for rule_index, row in enumerate(rule_rows):
        node_index = intern_path(row.path_signature)
        terminal_rules[node_index].append(rule_index)

    for row in override_rows:
        intern_path(row.path_signature)

    edge_rows: list[StoragePathEdgeRow] = []
    node_rows: list[StoragePathNodeRow] = []
    terminal_rule_indices: list[int] = []

    for node_index, children in enumerate(child_maps):
        first_edge_index = len(edge_rows)
        for tag_value, next_node_index in sorted(children.items()):
            edge_rows.append(
                StoragePathEdgeRow(tag_value=tag_value, next_node_index=next_node_index)
            )
        if first_edge_index >= 0xFFFF:
            raise RuntimeError("Storage path graph exceeded uint16_t edge index capacity")
        rules = terminal_rules[node_index]
        first_rule_index = len(terminal_rule_indices)
        terminal_rule_indices.extend(rules)
        node_rows.append(
            StoragePathNodeRow(
                incoming_tag=incoming_tags[node_index],
                parent_node_index=parent_indices[node_index],
                first_edge_index=first_edge_index,
                edge_count=len(children),
                first_rule_index=first_rule_index,
                rule_count=len(rules),
            )
        )

    return node_rows, edge_rows, terminal_rule_indices, path_node_indices


def build_ranges(rows: list[object], key_fn) -> list[KeyRange]:
    ranges: list[KeyRange] = []
    if not rows:
        return ranges
    begin = 0
    current_key = key_fn(rows[0])
    for index in range(1, len(rows) + 1):
        next_key = key_fn(rows[index]) if index < len(rows) else None
        if index == len(rows) or next_key != current_key:
            ranges.append(KeyRange(current_key, begin, index))
            if index < len(rows):
                begin = index
                current_key = next_key
    return ranges


def build_string_pool(values: list[str]) -> tuple[list[str], dict[str, int]]:
    pool: list[str] = []
    indices: dict[str, int] = {}
    for value in values:
        if value in indices:
            continue
        index = len(pool)
        if index >= 0xFFFF:
            raise RuntimeError("Storage component string pool exceeded uint16_t capacity")
        indices[value] = index
        pool.append(value)
    return pool, indices


def encode_component_kind(value: str) -> str:
    normalized = value.strip().lower()
    if normalized == "module":
        return "ComponentKind::Module"
    if normalized == "functional_group":
        return "ComponentKind::FunctionalGroup"
    return "ComponentKind::Unknown"


def encode_usage(value: str) -> str:
    normalized = value.strip().upper()
    if normalized == "M":
        return "ModuleUsage::Mandatory"
    if normalized == "C":
        return "ModuleUsage::Conditional"
    if normalized == "U":
        return "ModuleUsage::UserOption"
    return "ModuleUsage::Unknown"


def encode_type_designation(value: str) -> str:
    normalized = value.strip().upper()
    if normalized == "1":
        return "TypeDesignation::Type1"
    if normalized == "1C":
        return "TypeDesignation::Type1C"
    if normalized == "2":
        return "TypeDesignation::Type2"
    if normalized == "2C":
        return "TypeDesignation::Type2C"
    if normalized == "3":
        return "TypeDesignation::Type3"
    return "TypeDesignation::Unknown"


def encode_storage_context_kind(value: str) -> str:
    normalized = value.strip().lower()
    if normalized == "table":
        return "StorageContextKind::Table"
    return "StorageContextKind::Unknown"


def encode_storage_condition_op(value: str) -> str:
    mapping = {
        "external": "StorageConditionOp::External",
        "present": "StorageConditionOp::Present",
        "not_present": "StorageConditionOp::NotPresent",
        "empty": "StorageConditionOp::Empty",
        "eq_text": "StorageConditionOp::EqText",
        "ne_text": "StorageConditionOp::NeText",
        "value_eq_text": "StorageConditionOp::ValueEqText",
        "value_ne_text": "StorageConditionOp::ValueNeText",
        "greater_than": "StorageConditionOp::GreaterThan",
        "any_present": "StorageConditionOp::AnyPresent",
        "all_absent": "StorageConditionOp::AllAbsent",
        "tag_eq_any_tag": "StorageConditionOp::TagEqAnyTag",
        "tag_ne_any_tag": "StorageConditionOp::TagNeAnyTag",
        "and": "StorageConditionOp::And",
        "or": "StorageConditionOp::Or",
        "waveform_filter_kind_eq": "StorageConditionOp::WaveformFilterKindEq",
    }
    return mapping.get(value, "StorageConditionOp::Unknown")


def bool_literal(value: bool) -> str:
    return "true" if value else "false"


def append_array(lines: list[str], declaration: str, entries: list[str]) -> None:
    lines.append(f"{declaration} = {{")
    for entry in entries:
        lines.append(entry)
    lines.append("};")
    lines.append("")


def header_include_path(header_output: Path) -> str:
    parts = header_output.as_posix().split("/")
    if "include" in parts:
        idx = parts.index("include")
        suffix = "/".join(parts[idx + 1 :])
        if suffix:
            return suffix
    return header_output.name


def render_sopclass_iod_entries(rows: list[SopClassIodMapRow]) -> list[str]:
    entries: list[str] = []
    for idx, row in enumerate(rows):
        entries.append(
            f'    /* {idx:4d} */ SopClassStorageMapEntry{{{row.uid_index}u, "{escape(row.iod_xml_id)}", "{escape(row.iod_title)}", "{escape(row.part04_section_id)}", {bool_literal(row.retired)}}},'
        )
    return entries


def render_storage_component_string_entries(strings: list[str]) -> list[str]:
    entries: list[str] = []
    for idx, value in enumerate(strings):
        entries.append(f'    /* {idx:4d} */ "{escape(value)}",')
    return entries


def render_iod_component_entries(
    rows: list[IodComponentRow], string_indices: dict[str, int], program_indices: list[int]
) -> list[str]:
    entries: list[str] = []
    for idx, row in enumerate(rows):
        entries.append(
            f"    /* {idx:4d} */ StorageComponentRegistryEntry{{{string_indices[row.ie_name]}u, {string_indices[row.component_name]}u, {string_indices[row.component_section_id]}u, {program_indices[idx]}u, {encode_component_kind(row.entry_kind)}, {encode_usage(row.usage)}}},"
        )
    return entries


def render_component_rule_entries(rows: list[ComponentAttributeRuleRow]) -> list[str]:
    raise NotImplementedError("render_component_rule_entries requires path offsets")


def render_override_entries(rows: list[IodAttributeOverrideRow]) -> list[str]:
    raise NotImplementedError("render_override_entries requires path offsets")


def render_path_terminal_rule_index_entries(values: list[int], per_line: int = 12) -> list[str]:
    entries: list[str] = []
    for index in range(0, len(values), per_line):
        chunk = ", ".join(f"{value}u" for value in values[index : index + per_line])
        entries.append(f"    {chunk},")
    return entries


def render_storage_context_rule_index_entries(
    rows: list[StorageContextRuleIndexRow],
    rule_indices_by_component_row: dict[tuple[str, int], int],
) -> list[str]:
    values: list[int] = []
    for row in rows:
        key = (row.component_section_id, row.rule_row_index)
        if key not in rule_indices_by_component_row:
            raise RuntimeError(
                "Missing rule registry index for "
                f"{row.component_section_id!r} row_index={row.rule_row_index}"
            )
        values.append(rule_indices_by_component_row[key])
    return render_path_terminal_rule_index_entries(values)


def render_component_rule_entries(
    rows: list[ComponentAttributeRuleRow],
    component_string_indices: dict[str, int],
    path_node_indices: dict[str, int],
    condition_program_indices: list[int],
) -> list[str]:
    entries: list[str] = []
    for idx, row in enumerate(rows):
        entries.append(
            f"    /* {idx:5d} */ ComponentAttributeRuleEntry{{{component_string_indices[row.component_section_id]}u, {path_node_indices[row.path_signature]}u, 0x{row.tag_value:08X}u, {encode_type_designation(row.declared_type)}, {condition_program_indices[idx]}u, {bool_literal(row.may_be_present_otherwise)}}},"
        )
    return entries


def render_override_entries(
    rows: list[IodAttributeOverrideRow],
    path_node_indices: dict[str, int],
) -> list[str]:
    entries: list[str] = []
    for idx, row in enumerate(rows):
        entries.append(
            f'    /* {idx:4d} */ StorageAttributeOverrideEntry{{"{escape(row.iod_xml_id)}", {path_node_indices[row.path_signature]}u, 0x{row.tag_value:08X}u, "{escape(row.tag_text)}", {encode_type_designation(row.effective_type_override)}, "{escape(row.override_condition_text)}", "{escape(row.source_section_id)}", "{escape(row.reason)}"}},'
        )
    return entries


def render_path_node_entries(rows: list[StoragePathNodeRow]) -> list[str]:
    entries: list[str] = []
    for idx, row in enumerate(rows):
        parent_index = (
            "kInvalidStoragePathNodeIndex"
            if row.parent_node_index == 0xFFFF
            else f"{row.parent_node_index}u"
        )
        entries.append(
            f"    /* {idx:5d} */ StoragePathNodeEntry{{0x{row.incoming_tag:08X}u, {row.first_rule_index}u, {row.first_edge_index}u, {row.edge_count}u, {row.rule_count}u, {parent_index}}},"
        )
    return entries


def render_path_edge_entries(rows: list[StoragePathEdgeRow]) -> list[str]:
    entries: list[str] = []
    for idx, row in enumerate(rows):
        entries.append(
            f"    /* {idx:5d} */ StoragePathEdgeEntry{{0x{row.tag_value:08X}u, {row.next_node_index}u}},"
        )
    return entries


def render_storage_condition_string_entries(strings: list[str]) -> list[str]:
    entries: list[str] = []
    for idx, value in enumerate(strings):
        entries.append(f'    /* {idx:4d} */ "{escape(value)}",')
    return entries


def render_storage_condition_string_ref_entries(values: list[int], per_line: int = 16) -> list[str]:
    entries: list[str] = []
    for index in range(0, len(values), per_line):
        chunk = ", ".join(f"{value}u" for value in values[index : index + per_line])
        entries.append(f"    {chunk},")
    return entries


def render_storage_condition_tag_entries(values: list[int], per_line: int = 12) -> list[str]:
    entries: list[str] = []
    for index in range(0, len(values), per_line):
        chunk = ", ".join(f"0x{value:08X}u" for value in values[index : index + per_line])
        entries.append(f"    {chunk},")
    return entries


def render_storage_condition_program_entries(
    rows: list[StorageConditionProgramRow],
    condition_string_indices: dict[str, int],
) -> list[str]:
    entries: list[str] = []
    for idx, row in enumerate(rows):
        source_text_id = (
            "kInvalidStorageConditionStringId"
            if not row.source_text
            else f"{condition_string_indices[row.source_text]}u"
        )
        entries.append(
            "    /* {idx:5d} */ StorageConditionProgramEntry{{{arg0}u, {arg1}u, "
            "0x{tag_value:08X}u, {source_text_id}, {op}, {value_index}u}},".format(
                idx=idx,
                arg0=row.arg0,
                arg1=row.arg1,
                tag_value=row.tag_value,
                source_text_id=source_text_id,
                op=encode_storage_condition_op(row.op),
                value_index=row.value_index,
            )
        )
    return entries


def render_storage_context_entries(
    rows: list[StorageContextRow],
    string_indices: dict[str, int],
    transition_ranges: list[KeyRange],
    rule_ranges: list[KeyRange],
) -> list[str]:
    transition_range_by_key = {item.key: item for item in transition_ranges}
    rule_range_by_key = {item.key: item for item in rule_ranges}
    entries: list[str] = []
    for idx, row in enumerate(rows):
        context_identity = f"{row.component_section_id}\t{row.context_key}"
        transition_range = transition_range_by_key.get(context_identity)
        rule_range = rule_range_by_key.get(context_identity)
        first_transition = transition_range.begin if transition_range is not None else 0
        transition_count = (
            transition_range.end - transition_range.begin if transition_range is not None else 0
        )
        first_rule = rule_range.begin if rule_range is not None else 0
        rule_count = rule_range.end - rule_range.begin if rule_range is not None else 0
        entries.append(
            "    /* {idx:4d} */ StorageContextEntry{{{component_section_id_id}u, "
            "{context_key_id}u, {context_name_id}u, {first_transition}u, {first_rule}u, "
            "{transition_count}u, {rule_count}u, {context_kind}, {is_root}, {is_recursive}}},".format(
                idx=idx,
                component_section_id_id=string_indices[row.component_section_id],
                context_key_id=string_indices[row.context_key],
                context_name_id=string_indices[row.context_name],
                first_transition=first_transition,
                first_rule=first_rule,
                transition_count=transition_count,
                rule_count=rule_count,
                context_kind=encode_storage_context_kind(row.context_kind),
                is_root=bool_literal(row.is_root),
                is_recursive=bool_literal(row.is_recursive),
            )
        )
    return entries


def render_storage_context_transition_entries(
    rows: list[StorageContextTransitionRow],
    string_indices: dict[str, int],
    context_indices: dict[tuple[str, str], int],
) -> list[str]:
    entries: list[str] = []
    for idx, row in enumerate(rows):
        next_index = context_indices[(row.component_section_id, row.next_context_key)]
        entries.append(
            "    /* {idx:4d} */ StorageContextTransitionEntry{{{next_index}u, {condition_id}u, "
            "0x{sequence_tag:08X}u, {push_path}, {is_recursive}}},".format(
                idx=idx,
                next_index=next_index,
                condition_id=string_indices[row.transition_condition_text],
                sequence_tag=row.sequence_tag_value,
                push_path=bool_literal(row.push_path),
                is_recursive=bool_literal(row.is_recursive),
            )
        )
    return entries


def render_external_condition_report(
    component_rows: list[IodComponentRow],
    component_usage_program_indices: list[int],
    rule_rows: list[ComponentAttributeRuleRow],
    rule_condition_program_indices: list[int],
    condition_programs: list[StorageConditionProgramRow],
) -> str:
    component_metadata: dict[str, IodComponentRow] = {}
    for row in component_rows:
        component_metadata.setdefault(row.component_section_id, row)

    lines = ["\t".join(EXTERNAL_CONDITION_COLUMNS)]

    for index, row in enumerate(component_rows):
        program_index = component_usage_program_indices[index]
        if program_index == 0xFFFF or program_index >= len(condition_programs):
            continue
        program = condition_programs[program_index]
        if program.op != "external":
            continue
        lines.append(
            "\t".join(
                (
                    "component_usage",
                    row.component_section_id,
                    row.component_name,
                    row.ie_name,
                    row.entry_kind,
                    row.usage,
                    "",
                    "",
                    "",
                    str(program_index),
                    program.source_text,
                )
            )
        )

    for index, row in enumerate(rule_rows):
        program_index = rule_condition_program_indices[index]
        if program_index == 0xFFFF or program_index >= len(condition_programs):
            continue
        program = condition_programs[program_index]
        if program.op != "external":
            continue
        component_row = component_metadata.get(row.component_section_id)
        lines.append(
            "\t".join(
                (
                    "attribute_rule",
                    row.component_section_id,
                    component_row.component_name if component_row is not None else "",
                    component_row.ie_name if component_row is not None else "",
                    row.component_kind,
                    row.declared_type,
                    row.path_signature,
                    format_tag_text(row.tag_value),
                    f"0x{row.tag_value:08X}" if row.tag_value else "",
                    str(program_index),
                    program.source_text,
                )
            )
        )

    return "\n".join(lines) + "\n"


def render_range_entries(ranges: list[KeyRange]) -> list[str]:
    entries: list[str] = []
    for idx, item in enumerate(ranges):
        entries.append(
            f'    /* {idx:4d} */ KeyRangeEntry{{"{escape(item.key)}", {item.begin}u, {item.end}u}},'
        )
    return entries


def build_uid_index_to_sop_map(
    sop_rows: list[SopClassIodMapRow], uid_registry_count: int
) -> list[int]:
    mapping = [65535] * uid_registry_count
    for sop_index, row in enumerate(sop_rows):
        if row.uid_index < 0 or row.uid_index >= uid_registry_count:
            raise RuntimeError(
                f"uid_index {row.uid_index} for {row.uid_value!r} is out of UID registry range"
            )
        mapping[row.uid_index] = sop_index
    return mapping


def render_uid_index_map_entries(mapping: list[int]) -> list[str]:
    entries: list[str] = []
    for idx, value in enumerate(mapping):
        literal = (
            "kInvalidSopClassStorageMapIndex" if value == 65535 else f"{value}u"
        )
        entries.append(f"    /* {idx:4d} */ {literal},")
    return entries


def render_header(
    sop_rows: list[SopClassIodMapRow],
    uid_registry_count: int,
    component_strings: list[str],
    condition_strings: list[str],
    condition_string_refs: list[int],
    condition_tag_refs: list[int],
    condition_programs: list[StorageConditionProgramRow],
    component_rows: list[IodComponentRow],
    component_ranges: list[KeyRange],
    context_rows: list[StorageContextRow],
    context_ranges: list[KeyRange],
    context_transition_rows: list[StorageContextTransitionRow],
    context_rule_index_rows: list[StorageContextRuleIndexRow],
    path_nodes: list[StoragePathNodeRow],
    path_edges: list[StoragePathEdgeRow],
    path_terminal_rule_indices: list[int],
    rule_rows: list[ComponentAttributeRuleRow],
    rule_ranges: list[KeyRange],
    override_rows: list[IodAttributeOverrideRow],
    override_ranges: list[KeyRange],
    args: argparse.Namespace,
) -> str:
    lines = [
        HEADER_PREAMBLE.format(
            uid_source=args.uid_registry_source.as_posix(),
            sop_source=args.sopclass_iod_source.as_posix(),
            component_source=args.iod_component_source.as_posix(),
            rule_source=args.component_rule_source.as_posix(),
            override_source=args.override_source.as_posix(),
            context_source=args.context_source.as_posix(),
            context_transition_source=args.context_transition_source.as_posix(),
            context_rule_index_source=args.context_rule_index_source.as_posix(),
        ).rstrip("\n")
    ]

    declarations = [
        ("SopClassStorageMapEntry", "kSopClassStorageMap", len(sop_rows)),
        ("std::uint16_t", "kUidIndexToSopClassStorageMapIndex", uid_registry_count),
        ("std::string_view", "kStorageComponentStringTable", len(component_strings)),
        ("std::string_view", "kStorageConditionStringTable", len(condition_strings)),
        ("std::uint16_t", "kStorageConditionStringRefRegistry", len(condition_string_refs)),
        ("std::uint32_t", "kStorageConditionTagRegistry", len(condition_tag_refs)),
        ("StorageConditionProgramEntry", "kStorageConditionProgramRegistry", len(condition_programs)),
        ("StorageComponentRegistryEntry", "kStorageComponentRegistry", len(component_rows)),
        ("KeyRangeEntry", "kStorageComponentRanges", len(component_ranges)),
        ("StorageContextEntry", "kStorageContextRegistry", len(context_rows)),
        ("KeyRangeEntry", "kStorageContextRanges", len(context_ranges)),
        (
            "StorageContextTransitionEntry",
            "kStorageContextTransitionRegistry",
            len(context_transition_rows),
        ),
        (
            "std::uint32_t",
            "kStorageContextRuleIndexRegistry",
            len(context_rule_index_rows),
        ),
        ("StoragePathNodeEntry", "kStoragePathNodeRegistry", len(path_nodes)),
        ("StoragePathEdgeEntry", "kStoragePathEdgeRegistry", len(path_edges)),
        ("std::uint32_t", "kStoragePathTerminalRuleIndexRegistry", len(path_terminal_rule_indices)),
        ("ComponentAttributeRuleEntry", "kComponentAttributeRuleRegistry", len(rule_rows)),
        ("KeyRangeEntry", "kComponentAttributeRuleRanges", len(rule_ranges)),
        ("StorageAttributeOverrideEntry", "kStorageAttributeOverrideRegistry", len(override_rows)),
        ("KeyRangeEntry", "kStorageAttributeOverrideRanges", len(override_ranges)),
    ]

    for element_type, name, count in declarations:
        lines.append(f"inline constexpr std::size_t {name}Count = {count}u;")
        lines.append(f"extern const std::array<{element_type}, {name}Count> {name};")
        lines.append("")

    lines.append(HEADER_POSTAMBLE.rstrip("\n"))
    lines.append("")
    lines.append("} // namespace dicom::storage")
    lines.append("")
    return "\n".join(lines)


def render_source(
    sop_rows: list[SopClassIodMapRow],
    uid_index_to_sop_map: list[int],
    component_strings: list[str],
    component_string_indices: dict[str, int],
    condition_strings: list[str],
    condition_string_indices: dict[str, int],
    condition_string_refs: list[int],
    condition_tag_refs: list[int],
    condition_programs: list[StorageConditionProgramRow],
    component_usage_program_indices: list[int],
    component_rows: list[IodComponentRow],
    component_ranges: list[KeyRange],
    context_rows: list[StorageContextRow],
    context_ranges: list[KeyRange],
    context_transition_rows: list[StorageContextTransitionRow],
    context_transition_ranges: list[KeyRange],
    context_rule_index_rows: list[StorageContextRuleIndexRow],
    context_rule_index_ranges: list[KeyRange],
    context_indices: dict[tuple[str, str], int],
    rule_indices_by_component_row: dict[tuple[str, int], int],
    path_nodes: list[StoragePathNodeRow],
    path_edges: list[StoragePathEdgeRow],
    path_terminal_rule_indices: list[int],
    path_node_indices: dict[str, int],
    rule_rows: list[ComponentAttributeRuleRow],
    rule_condition_program_indices: list[int],
    rule_ranges: list[KeyRange],
    override_rows: list[IodAttributeOverrideRow],
    override_ranges: list[KeyRange],
    args: argparse.Namespace,
) -> str:
    lines = [
        SOURCE_PREAMBLE.format(
            uid_source=args.uid_registry_source.as_posix(),
            sop_source=args.sopclass_iod_source.as_posix(),
            component_source=args.iod_component_source.as_posix(),
            rule_source=args.component_rule_source.as_posix(),
            override_source=args.override_source.as_posix(),
            context_source=args.context_source.as_posix(),
            context_transition_source=args.context_transition_source.as_posix(),
            context_rule_index_source=args.context_rule_index_source.as_posix(),
            header_include=header_include_path(args.header_output),
        ).rstrip("\n")
    ]

    append_array(
        lines,
        "const std::array<SopClassStorageMapEntry, kSopClassStorageMapCount> kSopClassStorageMap",
        render_sopclass_iod_entries(sop_rows),
    )
    append_array(
        lines,
        "const std::array<std::uint16_t, kUidIndexToSopClassStorageMapIndexCount> kUidIndexToSopClassStorageMapIndex",
        render_uid_index_map_entries(uid_index_to_sop_map),
    )
    append_array(
        lines,
        "const std::array<std::string_view, kStorageComponentStringTableCount> kStorageComponentStringTable",
        render_storage_component_string_entries(component_strings),
    )
    append_array(
        lines,
        "const std::array<std::string_view, kStorageConditionStringTableCount> kStorageConditionStringTable",
        render_storage_condition_string_entries(condition_strings),
    )
    append_array(
        lines,
        "const std::array<std::uint16_t, kStorageConditionStringRefRegistryCount> kStorageConditionStringRefRegistry",
        render_storage_condition_string_ref_entries(condition_string_refs),
    )
    append_array(
        lines,
        "const std::array<std::uint32_t, kStorageConditionTagRegistryCount> kStorageConditionTagRegistry",
        render_storage_condition_tag_entries(condition_tag_refs),
    )
    append_array(
        lines,
        "const std::array<StorageConditionProgramEntry, kStorageConditionProgramRegistryCount> kStorageConditionProgramRegistry",
        render_storage_condition_program_entries(condition_programs, condition_string_indices),
    )
    append_array(
        lines,
        "const std::array<StorageComponentRegistryEntry, kStorageComponentRegistryCount> kStorageComponentRegistry",
        render_iod_component_entries(
            component_rows, component_string_indices, component_usage_program_indices
        ),
    )
    append_array(
        lines,
        "const std::array<KeyRangeEntry, kStorageComponentRangesCount> kStorageComponentRanges",
        render_range_entries(component_ranges),
    )
    append_array(
        lines,
        "const std::array<StorageContextEntry, kStorageContextRegistryCount> kStorageContextRegistry",
        render_storage_context_entries(
            context_rows,
            component_string_indices,
            context_transition_ranges,
            context_rule_index_ranges,
        ),
    )
    append_array(
        lines,
        "const std::array<KeyRangeEntry, kStorageContextRangesCount> kStorageContextRanges",
        render_range_entries(context_ranges),
    )
    append_array(
        lines,
        "const std::array<StorageContextTransitionEntry, kStorageContextTransitionRegistryCount> kStorageContextTransitionRegistry",
        render_storage_context_transition_entries(
            context_transition_rows,
            component_string_indices,
            context_indices,
        ),
    )
    append_array(
        lines,
        "const std::array<std::uint32_t, kStorageContextRuleIndexRegistryCount> kStorageContextRuleIndexRegistry",
        render_storage_context_rule_index_entries(
            context_rule_index_rows,
            rule_indices_by_component_row,
        ),
    )
    append_array(
        lines,
        "const std::array<StoragePathNodeEntry, kStoragePathNodeRegistryCount> kStoragePathNodeRegistry",
        render_path_node_entries(path_nodes),
    )
    append_array(
        lines,
        "const std::array<StoragePathEdgeEntry, kStoragePathEdgeRegistryCount> kStoragePathEdgeRegistry",
        render_path_edge_entries(path_edges),
    )
    append_array(
        lines,
        "const std::array<std::uint32_t, kStoragePathTerminalRuleIndexRegistryCount> kStoragePathTerminalRuleIndexRegistry",
        render_path_terminal_rule_index_entries(path_terminal_rule_indices),
    )
    append_array(
        lines,
        "const std::array<ComponentAttributeRuleEntry, kComponentAttributeRuleRegistryCount> kComponentAttributeRuleRegistry",
        render_component_rule_entries(
            rule_rows,
            component_string_indices,
            path_node_indices,
            rule_condition_program_indices,
        ),
    )
    append_array(
        lines,
        "const std::array<KeyRangeEntry, kComponentAttributeRuleRangesCount> kComponentAttributeRuleRanges",
        render_range_entries(rule_ranges),
    )
    append_array(
        lines,
        "const std::array<StorageAttributeOverrideEntry, kStorageAttributeOverrideRegistryCount> kStorageAttributeOverrideRegistry",
        render_override_entries(override_rows, path_node_indices),
    )
    append_array(
        lines,
        "const std::array<KeyRangeEntry, kStorageAttributeOverrideRangesCount> kStorageAttributeOverrideRanges",
        render_range_entries(override_ranges),
    )

    lines.append("} // namespace dicom::storage")
    lines.append("")
    return "\n".join(lines)


def main() -> None:
    args = parse_args()
    uid_indices, uid_registry_count = load_uid_indices(args.uid_registry_source)
    sop_rows = load_sopclass_iod_rows(args.sopclass_iod_source, uid_indices)
    uid_index_to_sop_map = build_uid_index_to_sop_map(sop_rows, uid_registry_count)
    component_rows = load_iod_component_rows(args.iod_component_source)
    condition_compiler = ConditionCompiler()
    component_usage_program_indices = [
        condition_compiler.compile_clause(extract_required_if_clause(row.usage_condition_text))
        for row in component_rows
    ]
    context_rows = load_storage_context_rows(args.context_source)
    context_transition_rows = load_storage_context_transition_rows(args.context_transition_source)
    context_rule_index_rows = load_storage_context_rule_index_rows(args.context_rule_index_source)
    component_string_values = [""]
    for row in component_rows:
        component_string_values.extend(
            [
                row.ie_name,
                row.component_name,
                row.component_section_id,
            ]
        )
    rule_rows = load_component_rule_rows(args.component_rule_source)
    rule_condition_program_indices = [
        condition_compiler.compile_clause(row.condition_text) for row in rule_rows
    ]
    for row in rule_rows:
        component_string_values.append(row.component_section_id)
    for row in context_rows:
        component_string_values.extend(
            [row.component_section_id, row.context_key, row.context_name]
        )
    for row in context_transition_rows:
        component_string_values.append(row.transition_condition_text)
    component_strings, component_string_indices = build_string_pool(component_string_values)
    condition_strings = condition_compiler.strings()
    condition_string_indices = {value: index for index, value in enumerate(condition_strings)}
    condition_string_refs = condition_compiler.string_ref_pool()
    condition_tag_refs = condition_compiler.tag_ref_pool()
    condition_programs = condition_compiler.programs()
    component_ranges = build_ranges(component_rows, lambda row: row.iod_xml_id)
    context_ranges = build_ranges(context_rows, lambda row: row.component_section_id)
    context_indices = {
        (row.component_section_id, row.context_key): index
        for index, row in enumerate(context_rows)
    }
    context_transition_ranges = build_ranges(
        context_transition_rows,
        lambda row: f"{row.component_section_id}\t{row.from_context_key}",
    )
    context_rule_index_ranges = build_ranges(
        context_rule_index_rows,
        lambda row: f"{row.component_section_id}\t{row.context_key}",
    )
    rule_ranges = build_ranges(rule_rows, lambda row: row.component_section_id)
    rule_indices_by_component_row = {
        (row.component_section_id, row.row_index): index
        for index, row in enumerate(rule_rows)
    }
    override_rows = load_override_rows(args.override_source)
    override_ranges = build_ranges(override_rows, lambda row: row.iod_xml_id)
    path_nodes, path_edges, path_terminal_rule_indices, path_node_indices = build_path_graph(
        rule_rows, override_rows
    )

    header_text = render_header(
        sop_rows,
        uid_registry_count,
        component_strings,
        condition_strings,
        condition_string_refs,
        condition_tag_refs,
        condition_programs,
        component_rows,
        component_ranges,
        context_rows,
        context_ranges,
        context_transition_rows,
        context_rule_index_rows,
        path_nodes,
        path_edges,
        path_terminal_rule_indices,
        rule_rows,
        rule_ranges,
        override_rows,
        override_ranges,
        args,
    )
    source_text = render_source(
        sop_rows,
        uid_index_to_sop_map,
        component_strings,
        component_string_indices,
        condition_strings,
        condition_string_indices,
        condition_string_refs,
        condition_tag_refs,
        condition_programs,
        component_usage_program_indices,
        component_rows,
        component_ranges,
        context_rows,
        context_ranges,
        context_transition_rows,
        context_transition_ranges,
        context_rule_index_rows,
        context_rule_index_ranges,
        context_indices,
        rule_indices_by_component_row,
        path_nodes,
        path_edges,
        path_terminal_rule_indices,
        path_node_indices,
        rule_rows,
        rule_condition_program_indices,
        rule_ranges,
        override_rows,
        override_ranges,
        args,
    )
    external_condition_report_text = render_external_condition_report(
        component_rows,
        component_usage_program_indices,
        rule_rows,
        rule_condition_program_indices,
        condition_programs,
    )

    args.header_output.parent.mkdir(parents=True, exist_ok=True)
    args.source_output.parent.mkdir(parents=True, exist_ok=True)
    args.external_condition_output.parent.mkdir(parents=True, exist_ok=True)
    write_text_if_changed(args.header_output, header_text)
    write_text_if_changed(args.source_output, source_text)
    write_text_if_changed(args.external_condition_output, external_condition_report_text)


if __name__ == "__main__":
    main()
