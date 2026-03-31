#pragma once

#include <algorithm>
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
    std::vector<ScopedRuleGroup> groups;
    std::unordered_map<std::uint16_t, std::size_t> group_lookup;
};

[[nodiscard]] inline PartitionedScopedRules build_partitioned_scoped_rules(
    std::span<const ComponentAttributeRuleEntry> rules) {
    PartitionedScopedRules partitioned;

    for (const auto& rule : rules) {
        const auto* path_node = find_storage_path_node_entry(rule.path_node_index);
        if (path_node == nullptr) {
            continue;
        }
        const auto scope_node_index = path_node->parent_node_index;
        if (partitioned.group_lookup.empty()) {
            partitioned.groups.reserve(rules.size());
            partitioned.group_lookup.reserve(rules.size());
        }
        const auto [it, inserted] =
            partitioned.group_lookup.try_emplace(scope_node_index, partitioned.groups.size());
        if (inserted) {
            partitioned.groups.push_back(ScopedRuleGroup{scope_node_index, {}});
        }
        partitioned.groups[it->second].rules.push_back(&rule);
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

[[nodiscard]] inline const ScopedRuleGroup* find_scoped_rule_group(
    const PartitionedScopedRules& partitioned,
    std::uint16_t scope_node_index) noexcept {
    const auto it = partitioned.group_lookup.find(scope_node_index);
    if (it == partitioned.group_lookup.end()) {
        return nullptr;
    }
    return &partitioned.groups[it->second];
}

struct TraversalComponentState {
    EffectiveModuleInfo module_info{};
    const PartitionedScopedRules* partitioned_rules{nullptr};
    bool use_context_traversal{false};
    StorageTraversalContext traversal{};
    std::vector<std::uint32_t> recursive_sequence_tags;
};

inline void append_unique_rule_candidate(
    std::vector<const ComponentAttributeRuleEntry*>& candidates,
    const ComponentAttributeRuleEntry* rule) {
    if (rule == nullptr) {
        return;
    }
    if (std::find(candidates.begin(), candidates.end(), rule) == candidates.end()) {
        candidates.push_back(rule);
    }
}

inline void append_context_rule_candidates(
    const TraversalComponentState& state,
    std::vector<const ComponentAttributeRuleEntry*>& candidates) {
    if (!state.use_context_traversal || state.module_info.evaluated.component == nullptr ||
        state.traversal.active_context_indices.empty()) {
        return;
    }
    const bool root_frame = state.traversal.path_stack.empty();
    for (const auto context_index : state.traversal.active_context_indices) {
        const auto* context = find_storage_context_entry(context_index);
        if (context == nullptr || (root_frame && !context->is_root)) {
            continue;
        }
        for (const auto rule_index : storage_context_rule_indices(*context)) {
            if (rule_index >= kComponentAttributeRuleRegistry.size()) {
                continue;
            }
            const auto* rule = &kComponentAttributeRuleRegistry[rule_index];
            if (rule->component_section_id() !=
                state.module_info.evaluated.component->component_section_id()) {
                continue;
            }
            bool local_to_current_recursive_frame = true;
            for (const auto recursive_sequence_tag : state.recursive_sequence_tags) {
                if (storage_path_node_contains_tag(rule->path_node_index, recursive_sequence_tag)) {
                    local_to_current_recursive_frame = false;
                    break;
                }
            }
            if (!local_to_current_recursive_frame) {
                continue;
            }
            append_unique_rule_candidate(candidates, rule);
        }
    }
}

[[nodiscard]] inline std::vector<std::uint32_t> find_recursive_context_sequence_tags(
    std::string_view component_section_id) {
    std::vector<std::uint32_t> tags;
    for (const auto& context : find_storage_contexts(component_section_id)) {
        for (const auto& transition : storage_context_transitions(context)) {
            if (!transition.is_recursive || transition.sequence_tag_value == 0) {
                continue;
            }
            if (std::find(tags.begin(), tags.end(), transition.sequence_tag_value) == tags.end()) {
                tags.push_back(transition.sequence_tag_value);
            }
        }
    }
    return tags;
}

[[nodiscard]] inline std::vector<TraversalComponentState> make_traversal_component_states(
    const SopClassStorageMapEntry* sop_class,
    const std::vector<EvaluatedComponentRef>& modules) {
    std::vector<TraversalComponentState> states;
    states.reserve(modules.size());
    for (const auto& module : modules) {
        if (!module.component) {
            continue;
        }
        const auto component_section_id = module.component->component_section_id();
        const auto root_context_indices = find_root_storage_context_indices(component_section_id);
        states.push_back(TraversalComponentState{
            EffectiveModuleInfo{sop_class, module},
            &partition_component_rules_by_scope(*module.component),
            !root_context_indices.empty(),
            root_context_indices.empty() ? StorageTraversalContext{}
                                         : make_storage_traversal_context(component_section_id),
            find_recursive_context_sequence_tags(component_section_id),
        });
    }
    return states;
}

template <typename EmitFrameFn>
inline void traverse_storage_frames(
    const DataSet& dataset,
    std::uint16_t scope_node_index,
    std::span<TraversalComponentState> states,
    EmitFrameFn&& emit_frame) {
    emit_frame(dataset, scope_node_index, states);

    for (const auto& element : dataset) {
        if (!element.is_present() || !element.vr().is_sequence()) {
            continue;
        }
        const auto* sequence = element.as_sequence();
        if (sequence == nullptr) {
            continue;
        }

        const auto child_scope_node_index =
            find_storage_path_child_node(scope_node_index, element.tag().value());

        std::vector<std::uint8_t> entered_contexts(states.size(), 0);
        for (int item_index = 0; item_index < sequence->size(); ++item_index) {
            const auto* item_dataset = sequence->get_dataset(static_cast<std::size_t>(item_index));
            if (item_dataset == nullptr) {
                continue;
            }

            for (std::size_t state_index = 0; state_index < states.size(); ++state_index) {
                auto& state = states[state_index];
                entered_contexts[state_index] = state.use_context_traversal &&
                                                enter_storage_sequence_item(
                                                    state.traversal, element.tag().value())
                                                    ? 1u
                                                    : 0u;
            }

            traverse_storage_frames(
                *item_dataset, child_scope_node_index, states, emit_frame);

            for (std::size_t state_index = states.size(); state_index-- > 0;) {
                if (entered_contexts[state_index] != 0) {
                    leave_storage_sequence_item(states[state_index].traversal);
                }
            }
        }
    }
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
    auto traversal_states =
        detail::make_traversal_component_states(classifier.sop_class_entry(), modules);
    for (const auto& state : traversal_states) {
        if (state.module_info.evaluated.component == nullptr) {
            continue;
        }
        attributes.reserve(
            attributes.size() +
            classifier.component_rules(*state.module_info.evaluated.component).size());
    }

    detail::traverse_storage_frames(
        dataset,
        0,
        std::span<detail::TraversalComponentState>{traversal_states},
        [&](const DataSet& scope_dataset,
            std::uint16_t scope_node_index,
            std::span<detail::TraversalComponentState> states) {
            detail::ConditionProgramCache program_cache;
            program_cache.states.reserve(16);
            for (auto& state : states) {
                if (state.module_info.evaluated.component == nullptr ||
                    state.partitioned_rules == nullptr) {
                    continue;
                }
                const bool skip_recursive_root_exact_rules =
                    state.use_context_traversal && scope_node_index == 0 &&
                    state.traversal.path_stack.empty();
                const auto* exact_group =
                    detail::find_scoped_rule_group(*state.partitioned_rules, scope_node_index);
                if (!skip_recursive_root_exact_rules && exact_group != nullptr) {
                    for (const auto* rule : exact_group->rules) {
                        const auto evaluated = evaluate_rule(
                            scope_dataset,
                            DeclaredRuleRef{state.module_info.evaluated.component, rule},
                            &program_cache,
                            context);
                        const auto info = EffectiveAttributeInfo{
                            classifier.sop_class_entry(),
                            state.module_info.evaluated.component,
                            rule,
                            evaluated.condition_state,
                            evaluated.effective_type,
                        };
                        if (!matches_effective_attribute_options(
                                state.module_info, info, options)) {
                            continue;
                        }
                        attributes.push_back(info);
                    }
                    continue;
                }

                std::vector<const ComponentAttributeRuleEntry*> candidates;
                detail::append_context_rule_candidates(state, candidates);
                for (const auto* rule : candidates) {
                    const auto evaluated = evaluate_rule(
                        scope_dataset,
                        DeclaredRuleRef{state.module_info.evaluated.component, rule},
                        &program_cache,
                        context);
                    const auto info = EffectiveAttributeInfo{
                        classifier.sop_class_entry(),
                        state.module_info.evaluated.component,
                        rule,
                        evaluated.condition_state,
                        evaluated.effective_type,
                    };
                    if (!matches_effective_attribute_options(state.module_info, info, options)) {
                        continue;
                    }
                    attributes.push_back(info);
                }
            }
        });
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
