#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "../dicom.h"
#include "storage_classifier.hpp"

namespace dicom::storage {

enum class ConditionState : std::uint8_t {
    NotConditional = 0,
    Active,
    Inactive,
    Indeterminate,
};

enum class EffectiveType : std::uint8_t {
    Unknown = 0,
    Type1,
    Type2,
    Type3,
    Prohibited,
};

enum class ConditionSource : std::uint8_t {
    AttributeType = 0,
    ComponentUsage,
};

enum class ConditionHandlingPolicy : std::uint8_t {
    BestEffort = 0,
    Strict,
};

[[nodiscard]] inline constexpr std::string_view to_string(ConditionState state) noexcept {
    switch (state) {
    case ConditionState::NotConditional:
        return "not_conditional";
    case ConditionState::Active:
        return "active";
    case ConditionState::Inactive:
        return "inactive";
    case ConditionState::Indeterminate:
    default:
        return "indeterminate";
    }
}

[[nodiscard]] inline constexpr std::string_view to_string(EffectiveType type) noexcept {
    switch (type) {
    case EffectiveType::Type1:
        return "type1";
    case EffectiveType::Type2:
        return "type2";
    case EffectiveType::Type3:
        return "type3";
    case EffectiveType::Prohibited:
        return "prohibited";
    case EffectiveType::Unknown:
    default:
        return "unknown";
    }
}

[[nodiscard]] inline constexpr std::string_view to_string(ConditionSource source) noexcept {
    switch (source) {
    case ConditionSource::AttributeType:
        return "attribute_type";
    case ConditionSource::ComponentUsage:
        return "component_usage";
    default:
        return "unknown";
    }
}

[[nodiscard]] inline constexpr std::string_view to_string(
    ConditionHandlingPolicy policy) noexcept {
    switch (policy) {
    case ConditionHandlingPolicy::BestEffort:
        return "best_effort";
    case ConditionHandlingPolicy::Strict:
        return "strict";
    default:
        return "unknown";
    }
}

struct EvaluatedRuleRef {
    const StorageComponentRegistryEntry* component{nullptr};
    const ComponentAttributeRuleEntry* rule{nullptr};
    ConditionState condition_state{ConditionState::Indeterminate};
    EffectiveType effective_type{EffectiveType::Unknown};

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return component != nullptr && rule != nullptr;
    }
};

struct EvaluatedComponentRef {
    const StorageComponentRegistryEntry* component{nullptr};
    ConditionState condition_state{ConditionState::Indeterminate};
    bool present_in_dataset{false};
    bool active{false};

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return component != nullptr;
    }
};

struct ConditionEvaluationRequest {
    ConditionSource source{ConditionSource::AttributeType};
    const StorageComponentRegistryEntry* component{nullptr};
    const ComponentAttributeRuleEntry* rule{nullptr};
    std::uint16_t program_index{kInvalidStorageConditionProgramIndex};
    std::string_view clause_text{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return component != nullptr;
    }
};

struct UnresolvedConditionInfo {
    ConditionSource source{ConditionSource::AttributeType};
    const StorageComponentRegistryEntry* component{nullptr};
    const ComponentAttributeRuleEntry* rule{nullptr};
    std::uint16_t program_index{kInvalidStorageConditionProgramIndex};
    std::string_view clause_text{};

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return component != nullptr;
    }
};

struct ConditionEvaluationReport {
    std::vector<UnresolvedConditionInfo> unresolved_conditions;

    [[nodiscard]] bool empty() const noexcept {
        return unresolved_conditions.empty();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return unresolved_conditions.size();
    }
};

using ExternalConditionEvaluator =
    ConditionState (*)(const DataSet& dataset,
                       const ConditionEvaluationRequest& request,
                       const void* user_data);

struct ConditionEvaluationContext {
    ExternalConditionEvaluator evaluator{nullptr};
    const void* user_data{nullptr};
    ConditionHandlingPolicy policy{ConditionHandlingPolicy::BestEffort};
    ConditionEvaluationReport* report{nullptr};

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return evaluator != nullptr || report != nullptr ||
               policy != ConditionHandlingPolicy::BestEffort;
    }
};

namespace detail {

struct PresentElementRegistry {
    std::vector<std::uint8_t> present_path_nodes;
};

struct ScopeDatasetRegistry {
    std::unordered_map<std::uint16_t, std::vector<const DataSet*>> scope_datasets_by_node;
};

inline void append_unresolved_condition(
    ConditionEvaluationReport* report,
    const UnresolvedConditionInfo& info) {
    if (report == nullptr || !info) {
        return;
    }
    report->unresolved_conditions.push_back(info);
}

[[nodiscard]] inline std::string ascii_lower(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char ch : text) {
        if (ch >= 'A' && ch <= 'Z') {
            out.push_back(static_cast<char>(ch - 'A' + 'a'));
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

[[nodiscard]] inline std::string_view trim_ascii(std::string_view text) noexcept {
    std::size_t begin = 0;
    while (begin < text.size() &&
           (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\n' ||
            text[begin] == '\r' || text[begin] == ',')) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin &&
           (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\n' ||
            text[end - 1] == '\r' || text[end - 1] == ',')) {
        --end;
    }
    return text.substr(begin, end - begin);
}

[[nodiscard]] inline std::string_view strip_balanced_quotes(std::string_view text) noexcept {
    text = trim_ascii(text);
    while (text.size() >= 2) {
        const char first = text.front();
        const char last = text.back();
        if (!((first == '"' && last == '"') || (first == '\'' && last == '\''))) {
            break;
        }
        text.remove_prefix(1);
        text.remove_suffix(1);
        text = trim_ascii(text);
    }
    return text;
}

[[nodiscard]] inline std::string_view waveform_filter_kind_from_path(
    std::uint16_t path_node_index) noexcept {
    if (storage_path_node_contains_tag(path_node_index, 0x003A0318u)) {
        return "HIGH_PASS";
    }
    if (storage_path_node_contains_tag(path_node_index, 0x003A0319u)) {
        return "LOW_PASS";
    }
    if (storage_path_node_contains_tag(path_node_index, 0x003A0321u)) {
        return "NOTCH";
    }
    return {};
}

[[nodiscard]] inline std::uint16_t find_storage_path_node(
    DataSetVisitPathRef path,
    Tag current_tag) noexcept {
    std::uint16_t node_index = 0;
    const auto steps = path.steps();
    for (const auto& step : steps) {
        node_index = find_storage_path_child_node(node_index, step.parent_sequence_tag.value());
        if (node_index == kInvalidStoragePathNodeIndex) {
            return node_index;
        }
    }
    return find_storage_path_child_node(node_index, current_tag.value());
}

[[nodiscard]] inline const DataElement& element_for_tag(const DataSet& dataset, Tag tag) {
    return dataset.get_dataelement(tag);
}

[[nodiscard]] inline bool element_present(const DataSet& dataset, Tag tag) {
    return element_for_tag(dataset, tag).is_present();
}

[[nodiscard]] inline bool element_empty(const DataSet& dataset, Tag tag) {
    const auto& element = element_for_tag(dataset, tag);
    return element.is_present() && element.length() == 0;
}

[[nodiscard]] inline bool element_has_text_value(
    const DataSet& dataset,
    Tag tag,
    std::string_view expected_upper) {
    const auto& element = element_for_tag(dataset, tag);
    if (!element.is_present()) {
        return false;
    }
    if (const auto values = element.to_string_views()) {
        for (const auto value : *values) {
            if (ascii_lower(trim_ascii(value)) == ascii_lower(trim_ascii(expected_upper))) {
                return true;
            }
        }
    }
    if (const auto value = element.to_string_view()) {
        return ascii_lower(trim_ascii(*value)) == ascii_lower(trim_ascii(expected_upper));
    }
    if (const auto value = element.to_utf8_string()) {
        return ascii_lower(trim_ascii(*value)) == ascii_lower(trim_ascii(expected_upper));
    }
    return false;
}

[[nodiscard]] inline bool element_has_any_text_value(
    const DataSet& dataset,
    Tag tag,
    std::span<const std::string_view> expected_values) {
    for (const auto expected : expected_values) {
        if (element_has_text_value(dataset, tag, expected)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] inline bool element_has_any_condition_string_value(
    const DataSet& dataset,
    Tag tag,
    std::span<const std::uint16_t> string_ids) {
    for (const auto string_id : string_ids) {
        if (element_has_text_value(dataset, tag, storage_condition_string(string_id))) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] inline bool element_has_any_tag_value(
    const DataSet& dataset,
    Tag tag,
    std::span<const std::uint32_t> expected_tags) {
    const auto& element = element_for_tag(dataset, tag);
    if (!element.is_present()) {
        return false;
    }
    if (const auto values = element.to_tag_vector()) {
        for (const auto value : *values) {
            for (const auto expected : expected_tags) {
                if (value.value() == expected) {
                    return true;
                }
            }
        }
    }
    if (const auto value = element.to_tag()) {
        for (const auto expected : expected_tags) {
            if (value->value() == expected) {
                return true;
            }
        }
    }
    return false;
}

[[nodiscard]] inline bool element_value_index_matches(
    const DataSet& dataset,
    Tag tag,
    std::size_t value_index_one_based,
    std::span<const std::uint16_t> expected_string_ids) {
    const auto& element = element_for_tag(dataset, tag);
    if (!element.is_present()) {
        return false;
    }
    if (const auto values = element.to_string_views()) {
        if (value_index_one_based == 0 || value_index_one_based > values->size()) {
            return false;
        }
        const auto actual = trim_ascii((*values)[value_index_one_based - 1]);
        for (const auto string_id : expected_string_ids) {
            const auto expected = storage_condition_string(string_id);
            if (ascii_lower(actual) == ascii_lower(trim_ascii(expected))) {
                return true;
            }
        }
    }
    return false;
}

[[nodiscard]] inline bool element_numeric_greater_than(
    const DataSet& dataset,
    Tag tag,
    long long threshold) {
    const auto& element = element_for_tag(dataset, tag);
    if (!element.is_present()) {
        return false;
    }
    if (const auto value = element.to_longlong()) {
        return *value > threshold;
    }
    if (const auto text = element.to_string_view()) {
        try {
            return std::stoll(std::string(trim_ascii(*text))) > threshold;
        } catch (...) {
            return false;
        }
    }
    return false;
}

[[nodiscard]] inline ConditionState combine_conditions(
    ConditionState lhs, ConditionState rhs) noexcept {
    if (lhs == ConditionState::Indeterminate || rhs == ConditionState::Indeterminate) {
        return ConditionState::Indeterminate;
    }
    if (lhs == ConditionState::Inactive || rhs == ConditionState::Inactive) {
        return ConditionState::Inactive;
    }
    if (lhs == ConditionState::Active && rhs == ConditionState::Active) {
        return ConditionState::Active;
    }
    if (lhs == ConditionState::NotConditional) {
        return rhs;
    }
    if (rhs == ConditionState::NotConditional) {
        return lhs;
    }
    return ConditionState::Indeterminate;
}

[[nodiscard]] inline ConditionState combine_any_conditions(
    ConditionState lhs, ConditionState rhs) noexcept {
    if (lhs == ConditionState::Active || rhs == ConditionState::Active) {
        return ConditionState::Active;
    }
    if (lhs == ConditionState::Indeterminate || rhs == ConditionState::Indeterminate) {
        return ConditionState::Indeterminate;
    }
    if (lhs == ConditionState::Inactive && rhs == ConditionState::Inactive) {
        return ConditionState::Inactive;
    }
    if (lhs == ConditionState::NotConditional) {
        return rhs;
    }
    if (rhs == ConditionState::NotConditional) {
        return lhs;
    }
    return ConditionState::Indeterminate;
}

[[nodiscard]] inline EffectiveType effective_type_from_type_designation(
    TypeDesignation declared_type,
    ConditionState condition_state,
    bool may_be_present_otherwise) noexcept {
    switch (declared_type) {
    case TypeDesignation::Type1:
        return EffectiveType::Type1;
    case TypeDesignation::Type2:
        return EffectiveType::Type2;
    case TypeDesignation::Type3:
        return EffectiveType::Type3;
    case TypeDesignation::Type1C:
        if (condition_state == ConditionState::Active) {
            return EffectiveType::Type1;
        }
        if (condition_state == ConditionState::Inactive) {
            return may_be_present_otherwise ? EffectiveType::Type3
                                            : EffectiveType::Prohibited;
        }
        return EffectiveType::Unknown;
    case TypeDesignation::Type2C:
        if (condition_state == ConditionState::Active) {
            return EffectiveType::Type2;
        }
        if (condition_state == ConditionState::Inactive) {
            return may_be_present_otherwise ? EffectiveType::Type3
                                            : EffectiveType::Prohibited;
        }
        return EffectiveType::Unknown;
    case TypeDesignation::Unknown:
    default:
        return EffectiveType::Unknown;
    }
}

[[nodiscard]] inline ConditionState evaluate_external_condition(
    const DataSet& dataset,
    const ConditionEvaluationRequest& request,
    const ConditionEvaluationContext& context = {}) {
    if (!request) {
        return ConditionState::Indeterminate;
    }
    if (context.evaluator == nullptr) {
        append_unresolved_condition(
            context.report,
            UnresolvedConditionInfo{
                request.source,
                request.component,
                request.rule,
                request.program_index,
                request.clause_text,
            });
        return ConditionState::Indeterminate;
    }
    const auto state = context.evaluator(dataset, request, context.user_data);
    if (state == ConditionState::Indeterminate) {
        append_unresolved_condition(
            context.report,
            UnresolvedConditionInfo{
                request.source,
                request.component,
                request.rule,
                request.program_index,
                request.clause_text,
            });
    }
    return state;
}

[[nodiscard]] inline ConditionState evaluate_condition_program(
    const DataSet& dataset,
    std::uint16_t program_index,
    std::uint16_t path_node_index,
    ConditionSource source,
    const StorageComponentRegistryEntry* component,
    const ComponentAttributeRuleEntry* rule,
    const ConditionEvaluationContext& context = {}) {
    const auto* program = find_storage_condition_program_entry(program_index);
    if (program == nullptr) {
        return ConditionState::Indeterminate;
    }

    switch (program->op) {
    case StorageConditionOp::Present:
        return element_present(dataset, Tag(program->tag_value)) ? ConditionState::Active
                                                                 : ConditionState::Inactive;
    case StorageConditionOp::NotPresent:
        return element_present(dataset, Tag(program->tag_value)) ? ConditionState::Inactive
                                                                 : ConditionState::Active;
    case StorageConditionOp::Empty:
        return element_empty(dataset, Tag(program->tag_value)) ? ConditionState::Active
                                                               : ConditionState::Inactive;
    case StorageConditionOp::EqText:
        return element_has_any_condition_string_value(
                   dataset, Tag(program->tag_value), storage_condition_string_refs(*program))
                   ? ConditionState::Active
                   : ConditionState::Inactive;
    case StorageConditionOp::NeText:
        return !element_has_any_condition_string_value(
                   dataset, Tag(program->tag_value), storage_condition_string_refs(*program))
                   ? ConditionState::Active
                   : ConditionState::Inactive;
    case StorageConditionOp::ValueEqText:
        return element_value_index_matches(
                   dataset,
                   Tag(program->tag_value),
                   static_cast<std::size_t>(program->value_index),
                   storage_condition_string_refs(*program))
                   ? ConditionState::Active
                   : ConditionState::Inactive;
    case StorageConditionOp::ValueNeText:
        return !element_value_index_matches(
                   dataset,
                   Tag(program->tag_value),
                   static_cast<std::size_t>(program->value_index),
                   storage_condition_string_refs(*program))
                   ? ConditionState::Active
                   : ConditionState::Inactive;
    case StorageConditionOp::GreaterThan:
        return element_numeric_greater_than(
                   dataset,
                   Tag(program->tag_value),
                   static_cast<long long>(static_cast<std::int32_t>(program->arg0)))
                   ? ConditionState::Active
                   : ConditionState::Inactive;
    case StorageConditionOp::AnyPresent:
        for (const auto tag_value : storage_condition_tag_values(*program)) {
            if (element_present(dataset, Tag(tag_value))) {
                return ConditionState::Active;
            }
        }
        return ConditionState::Inactive;
    case StorageConditionOp::AllAbsent:
        for (const auto tag_value : storage_condition_tag_values(*program)) {
            if (element_present(dataset, Tag(tag_value))) {
                return ConditionState::Inactive;
            }
        }
        return ConditionState::Active;
    case StorageConditionOp::TagEqAnyTag:
        return element_has_any_tag_value(
                   dataset, Tag(program->tag_value), storage_condition_tag_values(*program))
                   ? ConditionState::Active
                   : ConditionState::Inactive;
    case StorageConditionOp::TagNeAnyTag:
        return !element_has_any_tag_value(
                   dataset, Tag(program->tag_value), storage_condition_tag_values(*program))
                   ? ConditionState::Active
                   : ConditionState::Inactive;
    case StorageConditionOp::And:
        return combine_conditions(
            evaluate_condition_program(
                dataset,
                static_cast<std::uint16_t>(program->arg0),
                path_node_index,
                source,
                component,
                rule,
                context),
            evaluate_condition_program(
                dataset,
                static_cast<std::uint16_t>(program->arg1),
                path_node_index,
                source,
                component,
                rule,
                context));
    case StorageConditionOp::Or:
        return combine_any_conditions(
            evaluate_condition_program(
                dataset,
                static_cast<std::uint16_t>(program->arg0),
                path_node_index,
                source,
                component,
                rule,
                context),
            evaluate_condition_program(
                dataset,
                static_cast<std::uint16_t>(program->arg1),
                path_node_index,
                source,
                component,
                rule,
                context));
    case StorageConditionOp::WaveformFilterKindEq: {
        const auto actual = waveform_filter_kind_from_path(path_node_index);
        if (actual.empty()) {
            return ConditionState::Indeterminate;
        }
        for (const auto string_id : storage_condition_string_refs(*program)) {
            if (ascii_lower(actual) == ascii_lower(storage_condition_string(string_id))) {
                return ConditionState::Active;
            }
        }
        return ConditionState::Inactive;
    }
    case StorageConditionOp::External:
        return evaluate_external_condition(
            dataset,
            ConditionEvaluationRequest{
                source,
                component,
                rule,
                program_index,
                program->source_text(),
            },
            context);
    case StorageConditionOp::Unknown:
    default:
        return ConditionState::Indeterminate;
    }
}

[[nodiscard]] inline PresentElementRegistry collect_present_element_signatures(
    const DataSet& dataset) {
    PresentElementRegistry registry;
    registry.present_path_nodes.assign(kStoragePathNodeRegistry.size(), 0);
    dataset.visit([&](DataSetVisitPathRef path, const DataElement& element) {
        if (!element.is_present()) {
            return;
        }
        const auto path_node_index = find_storage_path_node(path, element.tag());
        if (path_node_index != kInvalidStoragePathNodeIndex) {
            registry.present_path_nodes[path_node_index] = 1;
        }
    });
    return registry;
}

inline void append_scope_dataset(
    ScopeDatasetRegistry& registry,
    std::uint16_t scope_node_index,
    const DataSet* dataset) {
    if (dataset == nullptr) {
        return;
    }
    registry.scope_datasets_by_node[scope_node_index].push_back(dataset);
}

inline void collect_scope_datasets_recursive(
    const DataSet& dataset,
    std::uint16_t scope_node_index,
    ScopeDatasetRegistry& registry) {
    append_scope_dataset(registry, scope_node_index, &dataset);
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
        if (child_scope_node_index == kInvalidStoragePathNodeIndex) {
            continue;
        }
        for (int item_index = 0; item_index < sequence->size(); ++item_index) {
            const auto* item_dataset = sequence->get_dataset(static_cast<std::size_t>(item_index));
            if (item_dataset == nullptr) {
                continue;
            }
            collect_scope_datasets_recursive(*item_dataset, child_scope_node_index, registry);
        }
    }
}

[[nodiscard]] inline ScopeDatasetRegistry collect_scope_datasets(const DataSet& dataset) {
    ScopeDatasetRegistry registry;
    collect_scope_datasets_recursive(dataset, 0, registry);
    return registry;
}

[[nodiscard]] inline const std::vector<const DataSet*>* find_scope_datasets(
    const ScopeDatasetRegistry& registry,
    std::uint16_t scope_node_index) noexcept {
    if (const auto it = registry.scope_datasets_by_node.find(scope_node_index);
        it != registry.scope_datasets_by_node.end()) {
        return &it->second;
    }
    return nullptr;
}

[[nodiscard]] inline bool component_present_in_dataset(
    const StorageClassifier& classifier,
    const StorageComponentRegistryEntry& component,
    const PresentElementRegistry& registry) {
    for (const auto& rule : classifier.component_rules(component)) {
        const auto path_node_index = static_cast<std::size_t>(rule.path_node_index);
        if (path_node_index < registry.present_path_nodes.size() &&
            registry.present_path_nodes[path_node_index] != 0) {
            return true;
        }
    }
    return false;
}

} // namespace detail

[[nodiscard]] inline EvaluatedRuleRef evaluate_rule(
    const DataSet& scope_dataset,
    DeclaredRuleRef declared_rule,
    const ConditionEvaluationContext& context = {}) {
    if (!declared_rule) {
        return {};
    }
    ConditionState condition_state = ConditionState::NotConditional;
    if (is_conditional_type_designation(declared_rule.rule->declared_type)) {
        if (declared_rule.rule->condition_program_index != kInvalidStorageConditionProgramIndex) {
            condition_state = detail::evaluate_condition_program(
                scope_dataset,
                declared_rule.rule->condition_program_index,
                declared_rule.rule->path_node_index,
                ConditionSource::AttributeType,
                declared_rule.component,
                declared_rule.rule,
                context);
        } else {
            condition_state = ConditionState::Indeterminate;
        }
    }
    return EvaluatedRuleRef{
        declared_rule.component,
        declared_rule.rule,
        condition_state,
        detail::effective_type_from_type_designation(
            declared_rule.rule->declared_type,
            condition_state,
            declared_rule.rule->may_be_present_otherwise),
    };
}

[[nodiscard]] inline std::vector<DeclaredRuleRef> find_rule_candidates(
    const StorageClassifier& classifier,
    std::uint16_t path_node_index,
    std::uint32_t tag_value) {
    std::vector<DeclaredRuleRef> matches;
    const auto* path_node = find_storage_path_node_entry(path_node_index);
    if (path_node == nullptr) {
        return matches;
    }
    for (const auto rule_index : storage_path_terminal_rule_indices(*path_node)) {
        if (rule_index >= kComponentAttributeRuleRegistry.size()) {
            continue;
        }
        const auto& rule = kComponentAttributeRuleRegistry[rule_index];
        if (rule.tag_value != tag_value) {
            continue;
        }
        if (const auto* component = classifier.find_component(rule.component_section_id())) {
            matches.push_back(DeclaredRuleRef{component, &rule});
        }
    }
    return matches;
}

[[nodiscard]] inline std::vector<EvaluatedRuleRef> classify_element(
    const StorageClassifier& classifier,
    DataSetVisitPathRef path,
    const DataElement& element,
    const ConditionEvaluationContext& context = {}) {
    std::vector<EvaluatedRuleRef> results;
    if (!element.parent()) {
        return results;
    }
    const auto path_node_index = detail::find_storage_path_node(path, element.tag());
    if (path_node_index == kInvalidStoragePathNodeIndex) {
        return results;
    }
    const auto candidates = find_rule_candidates(classifier, path_node_index, element.tag().value());
    results.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        results.push_back(evaluate_rule(*element.parent(), candidate, context));
    }
    return results;
}

[[nodiscard]] inline EvaluatedComponentRef evaluate_component(
    const DataSet& dataset,
    const StorageClassifier& classifier,
    const StorageComponentRegistryEntry& component,
    const detail::PresentElementRegistry* present_elements = nullptr,
    const ConditionEvaluationContext& context = {}) {
    const auto present_in_dataset = present_elements != nullptr
                                        ? detail::component_present_in_dataset(
                                              classifier, component, *present_elements)
                                        : false;
    if (component.usage == ModuleUsage::Mandatory) {
        return EvaluatedComponentRef{&component, ConditionState::Active, present_in_dataset, true};
    }
    if (component.usage == ModuleUsage::UserOption) {
        return EvaluatedComponentRef{
            &component,
            present_in_dataset ? ConditionState::Active : ConditionState::Inactive,
            present_in_dataset,
            present_in_dataset,
        };
    }

    ConditionState condition_state = ConditionState::Indeterminate;
    if (component.usage_condition_program_index != kInvalidStorageConditionProgramIndex) {
        condition_state = detail::evaluate_condition_program(
            dataset,
            component.usage_condition_program_index,
            kInvalidStoragePathNodeIndex,
            ConditionSource::ComponentUsage,
            &component,
            nullptr,
            context);
    }

    bool active = false;
    if (condition_state == ConditionState::Active) {
        active = true;
    } else if (condition_state == ConditionState::Inactive) {
        active = false;
    } else if (present_in_dataset &&
               context.policy != ConditionHandlingPolicy::Strict) {
        active = true;
    }
    return EvaluatedComponentRef{&component, condition_state, present_in_dataset, active};
}

[[nodiscard]] inline std::vector<EvaluatedComponentRef> evaluate_components(
    const DataSet& dataset,
    const StorageClassifier& classifier,
    const ConditionEvaluationContext& context = {}) {
    std::vector<EvaluatedComponentRef> evaluated;
    const auto present_elements = detail::collect_present_element_signatures(dataset);
    evaluated.reserve(classifier.components().size());
    for (const auto& component : classifier.components()) {
        evaluated.push_back(evaluate_component(
            dataset, classifier, component, &present_elements, context));
    }
    return evaluated;
}

} // namespace dicom::storage
