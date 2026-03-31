#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "storage_lookup.hpp"

namespace dicom::storage {

[[nodiscard]] inline constexpr std::string_view to_string(ComponentKind kind) noexcept {
    switch (kind) {
    case ComponentKind::Module:
        return "module";
    case ComponentKind::FunctionalGroup:
        return "functional_group";
    case ComponentKind::Unknown:
    default:
        return "unknown";
    }
}

[[nodiscard]] inline constexpr std::string_view to_string(ModuleUsage usage) noexcept {
    switch (usage) {
    case ModuleUsage::Mandatory:
        return "M";
    case ModuleUsage::Conditional:
        return "C";
    case ModuleUsage::UserOption:
        return "U";
    case ModuleUsage::Unknown:
    default:
        return "";
    }
}

[[nodiscard]] inline constexpr std::string_view to_string(TypeDesignation type) noexcept {
    switch (type) {
    case TypeDesignation::Type1:
        return "1";
    case TypeDesignation::Type1C:
        return "1C";
    case TypeDesignation::Type2:
        return "2";
    case TypeDesignation::Type2C:
        return "2C";
    case TypeDesignation::Type3:
        return "3";
    case TypeDesignation::Unknown:
    default:
        return "unknown";
    }
}

[[nodiscard]] inline constexpr bool is_conditional_type_designation(TypeDesignation type) noexcept {
    return type == TypeDesignation::Type1C || type == TypeDesignation::Type2C;
}

[[nodiscard]] inline constexpr bool is_unknown_type_designation(TypeDesignation type) noexcept {
    return type == TypeDesignation::Unknown;
}

[[nodiscard]] inline std::string_view rule_tag_text(
    const ComponentAttributeRuleEntry& rule) noexcept {
    if (const auto* entry = find_rule_dataelement_entry(rule)) {
        return entry->tag;
    }
    return {};
}

[[nodiscard]] inline std::string_view rule_attribute_name(
    const ComponentAttributeRuleEntry& rule) noexcept {
    if (const auto* entry = find_rule_dataelement_entry(rule)) {
        return entry->name;
    }
    return {};
}

[[nodiscard]] inline std::string_view rule_keyword(
    const ComponentAttributeRuleEntry& rule) noexcept {
    if (const auto* entry = find_rule_dataelement_entry(rule)) {
        return entry->keyword;
    }
    return {};
}

[[nodiscard]] inline std::string_view rule_component_section_id(
    const ComponentAttributeRuleEntry& rule) noexcept {
    return rule.component_section_id();
}

[[nodiscard]] inline std::string rule_path_signature(
    const ComponentAttributeRuleEntry& rule) {
    return format_path_signature(rule.path_node_index);
}

struct DeclaredRuleRef {
    const StorageComponentRegistryEntry* component{nullptr};
    const ComponentAttributeRuleEntry* rule{nullptr};

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return component != nullptr && rule != nullptr;
    }
};

struct StorageClassifierStats {
    std::size_t component_count{0};
    std::size_t attribute_rule_count{0};
    std::size_t unknown_type_rule_count{0};
    std::size_t conditional_type_rule_count{0};
    std::size_t override_count{0};
};

class StorageClassifier {
public:
    constexpr StorageClassifier() noexcept = default;

    [[nodiscard]] static inline std::optional<StorageClassifier> from_sop_class_uid(
        std::string_view uid_value) noexcept {
        if (const auto* entry = find_sop_class_storage_by_uid(uid_value)) {
            return StorageClassifier(entry);
        }
        return std::nullopt;
    }

    [[nodiscard]] static inline std::optional<StorageClassifier> from_sop_class_keyword(
        std::string_view keyword) noexcept {
        if (const auto* entry = find_sop_class_storage_by_keyword(keyword)) {
            return StorageClassifier(entry);
        }
        return std::nullopt;
    }

    [[nodiscard]] static inline std::optional<StorageClassifier> from_iod_xml_id(
        std::string_view iod_xml_id) noexcept {
        if (iod_xml_id.empty()) {
            return std::nullopt;
        }
        if (find_storage_components(iod_xml_id).empty()) {
            return std::nullopt;
        }
        return StorageClassifier(nullptr, iod_xml_id);
    }

    [[nodiscard]] constexpr bool valid() const noexcept {
        return !iod_xml_id().empty();
    }

    [[nodiscard]] constexpr const SopClassStorageMapEntry* sop_class_entry() const noexcept {
        return sop_class_entry_;
    }

    [[nodiscard]] constexpr std::uint16_t sop_class_uid_index() const noexcept {
        return sop_class_entry_ ? sop_class_entry_->uid_index : uid_lookup::kInvalidUidIndex;
    }

    [[nodiscard]] inline std::string_view sop_class_uid() const noexcept {
        if (sop_class_entry_) {
            if (const auto* uid_entry = find_sop_class_uid_entry(*sop_class_entry_)) {
                return uid_entry->value;
            }
        }
        return std::string_view{};
    }

    [[nodiscard]] inline std::string_view sop_class_keyword() const noexcept {
        if (sop_class_entry_) {
            if (const auto* uid_entry = find_sop_class_uid_entry(*sop_class_entry_)) {
                return uid_entry->keyword;
            }
        }
        return std::string_view{};
    }

    [[nodiscard]] inline std::string_view sop_class_name() const noexcept {
        if (sop_class_entry_) {
            if (const auto* uid_entry = find_sop_class_uid_entry(*sop_class_entry_)) {
                return uid_entry->name;
            }
        }
        return std::string_view{};
    }

    [[nodiscard]] constexpr std::string_view iod_xml_id() const noexcept {
        return !iod_xml_id_override_.empty()
                   ? iod_xml_id_override_
                   : (sop_class_entry_ ? sop_class_entry_->iod_xml_id : std::string_view{});
    }

    [[nodiscard]] constexpr std::string_view iod_title() const noexcept {
        return sop_class_entry_ ? sop_class_entry_->iod_title : std::string_view{};
    }

    [[nodiscard]] inline std::span<const StorageComponentRegistryEntry> components() const noexcept {
        return find_storage_components(iod_xml_id());
    }

    [[nodiscard]] inline std::span<const StorageAttributeOverrideEntry> overrides() const noexcept {
        return find_storage_attribute_overrides(iod_xml_id());
    }

    [[nodiscard]] inline const StorageComponentRegistryEntry* find_component(
        std::string_view component_section_id) const noexcept {
        for (const auto& component : components()) {
            if (component.component_section_id() == component_section_id) {
                return &component;
            }
        }
        return nullptr;
    }

    [[nodiscard]] inline std::span<const ComponentAttributeRuleEntry> component_rules(
        std::string_view component_section_id) const noexcept {
        return find_component_attribute_rules(component_section_id);
    }

    [[nodiscard]] inline std::span<const ComponentAttributeRuleEntry> component_rules(
        const StorageComponentRegistryEntry& component) const noexcept {
        return component_rules(component.component_section_id());
    }

    [[nodiscard]] inline DeclaredRuleRef find_rule(
        std::string_view component_section_id,
        std::string_view path_signature) const noexcept {
        const auto parsed_path = parse_path_signature(path_signature);
        if (!parsed_path) {
            return {};
        }
        return find_rule(component_section_id, std::span<const std::uint32_t>{*parsed_path});
    }

    [[nodiscard]] inline DeclaredRuleRef find_rule(
        std::string_view component_section_id,
        std::span<const std::uint32_t> path_tags) const noexcept {
        const auto* component = find_component(component_section_id);
        if (component == nullptr) {
            return {};
        }
        const auto path_node_index = find_storage_path_node(path_tags);
        if (path_node_index == kInvalidStoragePathNodeIndex) {
            return {};
        }
        for (const auto& rule : component_rules(*component)) {
            if (rule.path_node_index == path_node_index) {
                return DeclaredRuleRef{component, &rule};
            }
        }
        return {};
    }

    [[nodiscard]] inline std::vector<DeclaredRuleRef> find_rules_by_tag_value(
        std::uint32_t tag_value) const {
        std::vector<DeclaredRuleRef> matches;
        for (const auto& component : components()) {
            for (const auto& rule : component_rules(component)) {
                if (rule.tag_value == tag_value) {
                    matches.push_back(DeclaredRuleRef{&component, &rule});
                }
            }
        }
        return matches;
    }

    [[nodiscard]] inline std::vector<DeclaredRuleRef> find_rules_by_keyword(
        std::string_view keyword) const {
        std::vector<DeclaredRuleRef> matches;
        for (const auto& component : components()) {
            for (const auto& rule : component_rules(component)) {
                if (rule_keyword(rule) == keyword) {
                    matches.push_back(DeclaredRuleRef{&component, &rule});
                }
            }
        }
        return matches;
    }

    [[nodiscard]] inline bool has_unknown_type_designations() const noexcept {
        for (const auto& component : components()) {
            for (const auto& rule : component_rules(component)) {
                if (is_unknown_type_designation(rule.declared_type)) {
                    return true;
                }
            }
        }
        return false;
    }

    [[nodiscard]] inline StorageClassifierStats stats() const noexcept {
        StorageClassifierStats out{};
        out.component_count = components().size();
        out.override_count = overrides().size();
        for (const auto& component : components()) {
            for (const auto& rule : component_rules(component)) {
                ++out.attribute_rule_count;
                if (is_unknown_type_designation(rule.declared_type)) {
                    ++out.unknown_type_rule_count;
                }
                if (is_conditional_type_designation(rule.declared_type)) {
                    ++out.conditional_type_rule_count;
                }
            }
        }
        return out;
    }

private:
    constexpr explicit StorageClassifier(const SopClassStorageMapEntry* sop_class_entry) noexcept
        : sop_class_entry_(sop_class_entry) {}

    constexpr StorageClassifier(
        const SopClassStorageMapEntry* sop_class_entry,
        std::string_view iod_xml_id_override) noexcept
        : sop_class_entry_(sop_class_entry), iod_xml_id_override_(iod_xml_id_override) {}

    const SopClassStorageMapEntry* sop_class_entry_{nullptr};
    std::string_view iod_xml_id_override_{};
};

} // namespace dicom::storage
