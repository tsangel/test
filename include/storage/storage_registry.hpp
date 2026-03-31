// Auto-generated from:
//   - misc/dictionary/_uid_registry.tsv
//   - misc/dictionary/_sopclass_iod_map.tsv
//   - misc/dictionary/_iod_component_registry.tsv
//   - misc/dictionary/_component_attribute_rules.tsv
//   - misc/dictionary/_iod_attribute_overrides.tsv
//   - misc/dictionary/_storage_context_registry.tsv
//   - misc/dictionary/_storage_context_transition_registry.tsv
//   - misc/dictionary/_storage_context_rule_index_registry.tsv
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace dicom::storage {

enum class ComponentKind : std::uint8_t {
    Unknown = 0,
    Module,
    FunctionalGroup,
};

enum class ModuleUsage : std::uint8_t {
    Unknown = 0,
    Mandatory,
    Conditional,
    UserOption,
};

enum class TypeDesignation : std::uint8_t {
    Unknown = 0,
    Type1,
    Type1C,
    Type2,
    Type2C,
    Type3,
};

enum class StorageContextKind : std::uint8_t {
    Unknown = 0,
    Table,
};

enum class StorageConditionOp : std::uint8_t {
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
};

struct SopClassStorageMapEntry {
    std::uint16_t uid_index;
    std::string_view iod_xml_id;
    std::string_view iod_title;
    std::string_view part04_section_id;
    bool retired;
};

struct StorageComponentRegistryEntry {
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
};

struct StoragePathNodeEntry {
    std::uint32_t incoming_tag;
    std::uint32_t first_rule_index;
    std::uint16_t first_edge_index;
    std::uint16_t edge_count;
    std::uint16_t rule_count;
    std::uint16_t parent_node_index;
};

struct StoragePathEdgeEntry {
    std::uint32_t tag_value;
    std::uint16_t next_node_index;
};

struct StorageContextEntry {
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
};

struct StorageContextTransitionEntry {
    std::uint16_t next_context_index;
    std::uint16_t transition_condition_text_id;
    std::uint32_t sequence_tag_value;
    bool push_path;
    bool is_recursive;

    [[nodiscard]] std::string_view transition_condition_text() const noexcept;
};

struct StorageConditionProgramEntry {
    std::uint32_t arg0;
    std::uint32_t arg1;
    std::uint32_t tag_value;
    std::uint16_t source_text_id;
    StorageConditionOp op;
    std::uint8_t value_index;

    [[nodiscard]] std::string_view source_text() const noexcept;
};

struct ComponentAttributeRuleEntry {
    std::uint16_t component_section_id_id;
    std::uint16_t path_node_index;
    std::uint32_t tag_value;
    TypeDesignation declared_type;
    std::uint16_t condition_program_index;
    bool may_be_present_otherwise;

    [[nodiscard]] std::string_view component_section_id() const noexcept;
    [[nodiscard]] std::string_view condition_text() const noexcept;
};

struct StorageAttributeOverrideEntry {
    std::string_view iod_xml_id;
    std::uint16_t path_node_index;
    std::uint32_t tag_value;
    std::string_view tag_text;
    TypeDesignation effective_type_override;
    std::string_view override_condition_text;
    std::string_view source_section_id;
    std::string_view reason;
};

struct KeyRangeEntry {
    std::string_view key;
    std::uint32_t begin;
    std::uint32_t end;
};

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
inline constexpr std::size_t kSopClassStorageMapCount = 170u;
extern const std::array<SopClassStorageMapEntry, kSopClassStorageMapCount> kSopClassStorageMap;

inline constexpr std::size_t kUidIndexToSopClassStorageMapIndexCount = 465u;
extern const std::array<std::uint16_t, kUidIndexToSopClassStorageMapIndexCount> kUidIndexToSopClassStorageMapIndex;

inline constexpr std::size_t kStorageComponentStringTableCount = 2645u;
extern const std::array<std::string_view, kStorageComponentStringTableCount> kStorageComponentStringTable;

inline constexpr std::size_t kStorageConditionStringTableCount = 2010u;
extern const std::array<std::string_view, kStorageConditionStringTableCount> kStorageConditionStringTable;

inline constexpr std::size_t kStorageConditionStringRefRegistryCount = 444u;
extern const std::array<std::uint16_t, kStorageConditionStringRefRegistryCount> kStorageConditionStringRefRegistry;

inline constexpr std::size_t kStorageConditionTagRegistryCount = 122u;
extern const std::array<std::uint32_t, kStorageConditionTagRegistryCount> kStorageConditionTagRegistry;

inline constexpr std::size_t kStorageConditionProgramRegistryCount = 1671u;
extern const std::array<StorageConditionProgramEntry, kStorageConditionProgramRegistryCount> kStorageConditionProgramRegistry;

inline constexpr std::size_t kStorageComponentRegistryCount = 3845u;
extern const std::array<StorageComponentRegistryEntry, kStorageComponentRegistryCount> kStorageComponentRegistry;

inline constexpr std::size_t kStorageComponentRangesCount = 192u;
extern const std::array<KeyRangeEntry, kStorageComponentRangesCount> kStorageComponentRanges;

inline constexpr std::size_t kStorageContextRegistryCount = 2292u;
extern const std::array<StorageContextEntry, kStorageContextRegistryCount> kStorageContextRegistry;

inline constexpr std::size_t kStorageContextRangesCount = 517u;
extern const std::array<KeyRangeEntry, kStorageContextRangesCount> kStorageContextRanges;

inline constexpr std::size_t kStorageContextTransitionRegistryCount = 3420u;
extern const std::array<StorageContextTransitionEntry, kStorageContextTransitionRegistryCount> kStorageContextTransitionRegistry;

inline constexpr std::size_t kStorageContextRuleIndexRegistryCount = 72239u;
extern const std::array<std::uint32_t, kStorageContextRuleIndexRegistryCount> kStorageContextRuleIndexRegistry;

inline constexpr std::size_t kStoragePathNodeRegistryCount = 45753u;
extern const std::array<StoragePathNodeEntry, kStoragePathNodeRegistryCount> kStoragePathNodeRegistry;

inline constexpr std::size_t kStoragePathEdgeRegistryCount = 45752u;
extern const std::array<StoragePathEdgeEntry, kStoragePathEdgeRegistryCount> kStoragePathEdgeRegistry;

inline constexpr std::size_t kStoragePathTerminalRuleIndexRegistryCount = 72239u;
extern const std::array<std::uint32_t, kStoragePathTerminalRuleIndexRegistryCount> kStoragePathTerminalRuleIndexRegistry;

inline constexpr std::size_t kComponentAttributeRuleRegistryCount = 72239u;
extern const std::array<ComponentAttributeRuleEntry, kComponentAttributeRuleRegistryCount> kComponentAttributeRuleRegistry;

inline constexpr std::size_t kComponentAttributeRuleRangesCount = 514u;
extern const std::array<KeyRangeEntry, kComponentAttributeRuleRangesCount> kComponentAttributeRuleRanges;

inline constexpr std::size_t kStorageAttributeOverrideRegistryCount = 0u;
extern const std::array<StorageAttributeOverrideEntry, kStorageAttributeOverrideRegistryCount> kStorageAttributeOverrideRegistry;

inline constexpr std::size_t kStorageAttributeOverrideRangesCount = 0u;
extern const std::array<KeyRangeEntry, kStorageAttributeOverrideRangesCount> kStorageAttributeOverrideRanges;


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

} // namespace dicom::storage
