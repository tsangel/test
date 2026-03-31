#pragma once

#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "storage_dataset.hpp"
#include "storage_effective.hpp"

namespace dicom::storage {

struct DeclaredModuleListOptions {
    bool include_mandatory{true};
    bool include_conditional{true};
    bool include_user_option{true};
    std::string_view component_section_id{};
    std::string_view component_name{};
    std::string_view component_name_contains{};
    std::string_view ie_name{};
};

struct EffectiveModuleListOptions {
    bool include_mandatory{true};
    bool include_conditional{true};
    bool include_user_option{true};
    bool active_only{true};
    std::string_view component_section_id{};
    std::string_view component_name{};
    std::string_view component_name_contains{};
    std::string_view ie_name{};
};

struct DeclaredAttributeListOptions {
    bool include_unknown_types{true};
    bool include_conditional_types{true};
    bool include_user_option_components{true};
    std::string_view component_section_id{};
    std::string_view keyword{};
    std::string_view keyword_contains{};
    std::string_view path_prefix{};
    std::uint32_t tag_value{0};
};

struct EffectiveAttributeListOptions {
    bool include_unknown_effective_types{true};
    bool include_prohibited{true};
    bool include_conditional_declared_types{true};
    bool active_components_only{true};
    std::string_view component_section_id{};
    std::string_view keyword{};
    std::string_view keyword_contains{};
    std::string_view path_prefix{};
    std::uint32_t tag_value{0};
};

struct DeclaredModuleInfo {
    const SopClassStorageMapEntry* sop_class{nullptr};
    const StorageComponentRegistryEntry* component{nullptr};

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return component != nullptr;
    }
};

struct EffectiveModuleInfo {
    const SopClassStorageMapEntry* sop_class{nullptr};
    EvaluatedComponentRef evaluated{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return static_cast<bool>(evaluated);
    }
};

struct DeclaredAttributeInfo {
    const SopClassStorageMapEntry* sop_class{nullptr};
    const StorageComponentRegistryEntry* component{nullptr};
    const ComponentAttributeRuleEntry* rule{nullptr};

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return component != nullptr && rule != nullptr;
    }
};

struct EffectiveAttributeInfo {
    const SopClassStorageMapEntry* sop_class{nullptr};
    const StorageComponentRegistryEntry* component{nullptr};
    const ComponentAttributeRuleEntry* rule{nullptr};
    ConditionState condition_state{ConditionState::Indeterminate};
    EffectiveType effective_type{EffectiveType::Unknown};

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return component != nullptr && rule != nullptr;
    }
};

namespace detail {

struct ScopedRuleGroup {
    std::uint16_t scope_node_index{0};
    std::vector<const ComponentAttributeRuleEntry*> rules;
};

struct PartitionedScopedRules {
    std::vector<const ComponentAttributeRuleEntry*> root_rules;
    std::vector<ScopedRuleGroup> nested_groups;
};

[[nodiscard]] inline PartitionedScopedRules build_partitioned_scoped_rules(
    std::span<const ComponentAttributeRuleEntry> rules) {
    PartitionedScopedRules partitioned;
    partitioned.root_rules.reserve(rules.size());

    std::unordered_map<std::uint16_t, std::size_t> nested_group_indices;

    for (const auto& rule : rules) {
        const auto* path_node = find_storage_path_node_entry(rule.path_node_index);
        if (path_node == nullptr) {
            continue;
        }
        const auto scope_node_index = path_node->parent_node_index;
        if (scope_node_index == 0) {
            partitioned.root_rules.push_back(&rule);
            continue;
        }
        if (nested_group_indices.empty()) {
            partitioned.nested_groups.reserve(rules.size());
            nested_group_indices.reserve(rules.size());
        }
        const auto [it, inserted] = nested_group_indices.try_emplace(
            scope_node_index, partitioned.nested_groups.size());
        if (inserted) {
            partitioned.nested_groups.push_back(ScopedRuleGroup{scope_node_index, {}});
        }
        partitioned.nested_groups[it->second].rules.push_back(&rule);
    }

    return partitioned;
}

[[nodiscard]] inline const PartitionedScopedRules& partition_component_rules_by_scope(
    const StorageComponentRegistryEntry& component) {
    static const auto* cache = [] {
        auto* partitioned_rules =
            new std::vector<PartitionedScopedRules>(kStorageComponentRegistry.size());
        for (std::size_t index = 0; index < kStorageComponentRegistry.size(); ++index) {
            const auto& component_entry = kStorageComponentRegistry[index];
            (*partitioned_rules)[index] =
                build_partitioned_scoped_rules(find_component_attribute_rules(
                    component_entry.component_section_id()));
        }
        return partitioned_rules;
    }();

    const auto* begin = kStorageComponentRegistry.data();
    const auto* end = begin + kStorageComponentRegistry.size();
    if (&component < begin || &component >= end) {
        static const auto* empty = new PartitionedScopedRules{};
        return *empty;
    }

    return (*cache)[static_cast<std::size_t>(&component - begin)];
}

} // namespace detail

[[nodiscard]] inline bool matches_declared_module_options(
    const StorageComponentRegistryEntry& component,
    const DeclaredModuleListOptions& options) noexcept {
    if (!options.include_mandatory && component.usage == ModuleUsage::Mandatory) {
        return false;
    }
    if (!options.include_conditional && component.usage == ModuleUsage::Conditional) {
        return false;
    }
    if (!options.include_user_option && component.usage == ModuleUsage::UserOption) {
        return false;
    }
    if (!options.component_section_id.empty() &&
        component.component_section_id() != options.component_section_id) {
        return false;
    }
    if (!options.component_name.empty() && component.component_name() != options.component_name) {
        return false;
    }
    if (!options.component_name_contains.empty() &&
        component.component_name().find(options.component_name_contains) == std::string_view::npos) {
        return false;
    }
    if (!options.ie_name.empty() && component.ie_name() != options.ie_name) {
        return false;
    }
    return true;
}

[[nodiscard]] inline bool matches_effective_module_options(
    const EvaluatedComponentRef& component,
    const EffectiveModuleListOptions& options) noexcept {
    if (!component.component) {
        return false;
    }
    if (options.active_only && !component.active) {
        return false;
    }
    return matches_declared_module_options(
        *component.component,
        DeclaredModuleListOptions{
            options.include_mandatory,
            options.include_conditional,
            options.include_user_option,
            options.component_section_id,
            options.component_name,
            options.component_name_contains,
            options.ie_name,
        });
}

[[nodiscard]] inline bool matches_declared_attribute_options(
    const StorageComponentRegistryEntry& component,
    const ComponentAttributeRuleEntry& rule,
    const DeclaredAttributeListOptions& options) noexcept {
    if (!options.include_user_option_components && component.usage == ModuleUsage::UserOption) {
        return false;
    }
    if (!options.include_unknown_types && is_unknown_type_designation(rule.declared_type)) {
        return false;
    }
    if (!options.include_conditional_types &&
        is_conditional_type_designation(rule.declared_type)) {
        return false;
    }
    if (!options.component_section_id.empty() &&
        component.component_section_id() != options.component_section_id) {
        return false;
    }
    const auto keyword = rule_keyword(rule);
    if (!options.keyword.empty() && keyword != options.keyword) {
        return false;
    }
    if (!options.keyword_contains.empty() &&
        keyword.find(options.keyword_contains) == std::string_view::npos) {
        return false;
    }
    if (!options.path_prefix.empty() &&
        !path_starts_with_signature_text(rule.path_node_index, options.path_prefix)) {
        return false;
    }
    if (options.tag_value != 0 && rule.tag_value != options.tag_value) {
        return false;
    }
    return true;
}

[[nodiscard]] inline bool matches_effective_attribute_options(
    const EffectiveModuleInfo& module,
    const EffectiveAttributeInfo& attribute,
    const EffectiveAttributeListOptions& options) noexcept {
    if (!attribute.component || !attribute.rule) {
        return false;
    }
    if (options.active_components_only && !module.evaluated.active) {
        return false;
    }
    if (!options.include_unknown_effective_types &&
        attribute.effective_type == EffectiveType::Unknown) {
        return false;
    }
    if (!options.include_prohibited &&
        attribute.effective_type == EffectiveType::Prohibited) {
        return false;
    }
    if (!options.include_conditional_declared_types &&
        is_conditional_type_designation(attribute.rule->declared_type)) {
        return false;
    }
    if (!options.component_section_id.empty() &&
        attribute.component->component_section_id() != options.component_section_id) {
        return false;
    }
    const auto keyword = rule_keyword(*attribute.rule);
    if (!options.keyword.empty() && keyword != options.keyword) {
        return false;
    }
    if (!options.keyword_contains.empty() &&
        keyword.find(options.keyword_contains) == std::string_view::npos) {
        return false;
    }
    if (!options.path_prefix.empty() &&
        !path_starts_with_signature_text(attribute.rule->path_node_index, options.path_prefix)) {
        return false;
    }
    if (options.tag_value != 0 && attribute.rule->tag_value != options.tag_value) {
        return false;
    }
    return true;
}

[[nodiscard]] inline std::vector<DeclaredModuleInfo> list_modules(
    const StorageClassifier& classifier,
    const DeclaredModuleListOptions& options = {}) {
    std::vector<DeclaredModuleInfo> modules;
    modules.reserve(classifier.components().size());
    for (const auto& component : classifier.components()) {
        if (!matches_declared_module_options(component, options)) {
            continue;
        }
        modules.push_back(DeclaredModuleInfo{classifier.sop_class_entry(), &component});
    }
    return modules;
}

[[nodiscard]] inline std::vector<DeclaredModuleInfo> list_modules(
    std::string_view sop_class_uid,
    const DeclaredModuleListOptions& options = {}) {
    if (const auto classifier = StorageClassifier::from_sop_class_uid(sop_class_uid)) {
        return list_modules(*classifier, options);
    }
    return {};
}

[[nodiscard]] inline std::vector<DeclaredModuleInfo> list_modules_by_keyword(
    std::string_view sop_class_keyword,
    const DeclaredModuleListOptions& options = {}) {
    if (const auto classifier = StorageClassifier::from_sop_class_keyword(sop_class_keyword)) {
        return list_modules(*classifier, options);
    }
    return {};
}

[[nodiscard]] inline std::vector<DeclaredAttributeInfo> list_attributes(
    const StorageClassifier& classifier,
    const DeclaredAttributeListOptions& options = {}) {
    std::vector<DeclaredAttributeInfo> attributes;
    for (const auto& component : classifier.components()) {
        const auto rules = classifier.component_rules(component);
        attributes.reserve(attributes.size() + rules.size());
        for (const auto& rule : rules) {
            if (!matches_declared_attribute_options(component, rule, options)) {
                continue;
            }
            attributes.push_back(
                DeclaredAttributeInfo{classifier.sop_class_entry(), &component, &rule});
        }
    }
    return attributes;
}

[[nodiscard]] inline std::vector<DeclaredAttributeInfo> list_attributes(
    const StorageClassifier& classifier,
    bool active_components_only) {
    auto options = DeclaredAttributeListOptions{};
    options.include_user_option_components = !active_components_only;
    return list_attributes(classifier, options);
}

[[nodiscard]] inline std::vector<DeclaredAttributeInfo> list_attributes(
    std::string_view sop_class_uid,
    const DeclaredAttributeListOptions& options = {}) {
    if (const auto classifier = StorageClassifier::from_sop_class_uid(sop_class_uid)) {
        return list_attributes(*classifier, options);
    }
    return {};
}

[[nodiscard]] inline std::vector<DeclaredAttributeInfo> list_attributes(
    std::string_view sop_class_uid,
    bool active_components_only) {
    auto options = DeclaredAttributeListOptions{};
    options.include_user_option_components = !active_components_only;
    return list_attributes(sop_class_uid, options);
}

[[nodiscard]] inline std::vector<DeclaredAttributeInfo> list_attributes_by_keyword(
    std::string_view sop_class_keyword,
    const DeclaredAttributeListOptions& options = {}) {
    if (const auto classifier = StorageClassifier::from_sop_class_keyword(sop_class_keyword)) {
        return list_attributes(*classifier, options);
    }
    return {};
}

[[nodiscard]] inline std::vector<DeclaredAttributeInfo> list_attributes_by_keyword(
    std::string_view sop_class_keyword,
    bool active_components_only) {
    auto options = DeclaredAttributeListOptions{};
    options.include_user_option_components = !active_components_only;
    return list_attributes_by_keyword(sop_class_keyword, options);
}

[[nodiscard]] inline std::vector<EffectiveModuleInfo> list_effective_modules(
    const StorageClassifier& classifier,
    const DataSet& dataset,
    const EffectiveModuleListOptions& options = {},
    const ConditionEvaluationContext& context = {}) {
    std::vector<EffectiveModuleInfo> modules;
    const auto evaluated = evaluate_components(dataset, classifier, context);
    modules.reserve(evaluated.size());
    for (const auto& component : evaluated) {
        if (!matches_effective_module_options(component, options)) {
            continue;
        }
        modules.push_back(EffectiveModuleInfo{classifier.sop_class_entry(), component});
    }
    return modules;
}

[[nodiscard]] inline std::vector<EffectiveModuleInfo> list_effective_modules(
    const StorageClassifier& classifier,
    const DataSet& dataset,
    bool active_only,
    const ConditionEvaluationContext& context = {}) {
    auto options = EffectiveModuleListOptions{};
    options.active_only = active_only;
    return list_effective_modules(classifier, dataset, options, context);
}

[[nodiscard]] inline std::vector<EffectiveModuleInfo> list_effective_modules(
    const DataSet& dataset,
    const EffectiveModuleListOptions& options = {},
    const ConditionEvaluationContext& context = {}) {
    if (const auto classifier = make_storage_classifier(dataset)) {
        return list_effective_modules(*classifier, dataset, options, context);
    }
    return {};
}

[[nodiscard]] inline std::vector<EffectiveModuleInfo> list_effective_modules(
    const DataSet& dataset,
    bool active_only,
    const ConditionEvaluationContext& context = {}) {
    auto options = EffectiveModuleListOptions{};
    options.active_only = active_only;
    return list_effective_modules(dataset, options, context);
}

[[nodiscard]] inline std::vector<EffectiveAttributeInfo> list_effective_attributes(
    const StorageClassifier& classifier,
    const DataSet& dataset,
    const EffectiveAttributeListOptions& options = {},
    const ConditionEvaluationContext& context = {}) {
    std::vector<EffectiveAttributeInfo> attributes;
    const auto modules = evaluate_components(dataset, classifier, context);
    detail::ScopeDatasetRegistry scope_datasets;
    bool scope_datasets_collected = false;
    for (const auto& module : modules) {
        if (!module.component) {
            continue;
        }
        const auto rules = classifier.component_rules(*module.component);
        const auto& partitioned_rules = detail::partition_component_rules_by_scope(*module.component);
        attributes.reserve(attributes.size() + rules.size());
        const auto module_info = EffectiveModuleInfo{classifier.sop_class_entry(), module};
        for (const auto* rule : partitioned_rules.root_rules) {
            if (rule == nullptr) {
                continue;
            }
            const auto evaluated =
                evaluate_rule(dataset, DeclaredRuleRef{module.component, rule}, context);
            const auto info = EffectiveAttributeInfo{
                classifier.sop_class_entry(),
                module.component,
                rule,
                evaluated.condition_state,
                evaluated.effective_type,
            };
            if (!matches_effective_attribute_options(module_info, info, options)) {
                continue;
            }
            attributes.push_back(info);
        }

        for (const auto& group : partitioned_rules.nested_groups) {
            if (!scope_datasets_collected) {
                scope_datasets = detail::collect_scope_datasets(dataset);
                scope_datasets_collected = true;
            }
            const auto* scopes = detail::find_scope_datasets(scope_datasets, group.scope_node_index);
            if (scopes == nullptr || scopes->empty()) {
                continue;
            }

            for (const auto* rule : group.rules) {
                if (rule == nullptr) {
                    continue;
                }
                for (const auto* scope_dataset : *scopes) {
                    if (scope_dataset == nullptr) {
                        continue;
                    }
                    const auto evaluated =
                        evaluate_rule(*scope_dataset, DeclaredRuleRef{module.component, rule}, context);
                    const auto info = EffectiveAttributeInfo{
                        classifier.sop_class_entry(),
                        module.component,
                        rule,
                        evaluated.condition_state,
                        evaluated.effective_type,
                    };
                    if (!matches_effective_attribute_options(module_info, info, options)) {
                        continue;
                    }
                    attributes.push_back(info);
                }
            }
        }
    }
    return attributes;
}

[[nodiscard]] inline std::vector<EffectiveAttributeInfo> list_effective_attributes(
    const StorageClassifier& classifier,
    const DataSet& dataset,
    bool active_components_only,
    const ConditionEvaluationContext& context = {}) {
    auto options = EffectiveAttributeListOptions{};
    options.active_components_only = active_components_only;
    return list_effective_attributes(classifier, dataset, options, context);
}

[[nodiscard]] inline std::vector<EffectiveAttributeInfo> list_effective_attributes(
    const DataSet& dataset,
    const EffectiveAttributeListOptions& options = {},
    const ConditionEvaluationContext& context = {}) {
    if (const auto classifier = make_storage_classifier(dataset)) {
        return list_effective_attributes(*classifier, dataset, options, context);
    }
    return {};
}

[[nodiscard]] inline std::vector<EffectiveAttributeInfo> list_effective_attributes(
    const DataSet& dataset,
    bool active_components_only,
    const ConditionEvaluationContext& context = {}) {
    auto options = EffectiveAttributeListOptions{};
    options.active_components_only = active_components_only;
    return list_effective_attributes(dataset, options, context);
}

[[nodiscard]] inline ConditionEvaluationReport collect_condition_issues(
    const StorageClassifier& classifier,
    const DataSet& dataset,
    ConditionHandlingPolicy policy = ConditionHandlingPolicy::BestEffort,
    const ConditionEvaluationContext& base_context = {}) {
    ConditionEvaluationReport report;
    auto context = base_context;
    context.policy = policy;
    context.report = &report;

    auto options = EffectiveAttributeListOptions{};
    options.active_components_only = false;
    options.include_prohibited = true;
    options.include_unknown_effective_types = true;
    options.include_conditional_declared_types = true;
    (void)list_effective_attributes(classifier, dataset, options, context);
    return report;
}

[[nodiscard]] inline ConditionEvaluationReport collect_condition_issues(
    const DataSet& dataset,
    ConditionHandlingPolicy policy = ConditionHandlingPolicy::BestEffort,
    const ConditionEvaluationContext& base_context = {}) {
    if (const auto classifier = make_storage_classifier(dataset)) {
        return collect_condition_issues(*classifier, dataset, policy, base_context);
    }
    return {};
}

} // namespace dicom::storage
