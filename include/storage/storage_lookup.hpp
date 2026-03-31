#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../dataelement_lookup_detail.hpp"
#include "../uid_lookup_detail.hpp"
#include "storage_registry.hpp"

namespace dicom::storage {

[[nodiscard]] inline const UidEntry* find_sop_class_uid_entry(
    const SopClassStorageMapEntry& entry) noexcept {
    return uid_lookup::entry_from_index(entry.uid_index);
}

[[nodiscard]] inline const SopClassStorageMapEntry* find_sop_class_storage_by_uid_index(
    std::uint16_t uid_index) noexcept {
    if (uid_index >= kUidIndexToSopClassStorageMapIndex.size()) {
        return nullptr;
    }
    const auto sop_index = kUidIndexToSopClassStorageMapIndex[uid_index];
    if (sop_index == kInvalidSopClassStorageMapIndex || sop_index >= kSopClassStorageMap.size()) {
        return nullptr;
    }
    return &kSopClassStorageMap[sop_index];
}

[[nodiscard]] inline const SopClassStorageMapEntry* find_sop_class_storage_by_uid(
    std::string_view uid_value) noexcept {
    const auto uid_index = uid_lookup::uid_index_from_value(uid_value);
    return uid_index == uid_lookup::kInvalidUidIndex ? nullptr
                                                     : find_sop_class_storage_by_uid_index(uid_index);
}

[[nodiscard]] inline const SopClassStorageMapEntry* find_sop_class_storage_by_keyword(
    std::string_view keyword) noexcept {
    const auto uid_index = uid_lookup::uid_index_from_keyword(keyword);
    return uid_index == uid_lookup::kInvalidUidIndex ? nullptr
                                                     : find_sop_class_storage_by_uid_index(uid_index);
}

[[nodiscard]] inline const DataElementEntry* find_dataelement_entry(
    std::uint32_t tag_value) noexcept {
    return lookup::tag_to_entry(tag_value);
}

[[nodiscard]] inline const DataElementEntry* find_rule_dataelement_entry(
    const ComponentAttributeRuleEntry& rule) noexcept {
    return find_dataelement_entry(rule.tag_value);
}

[[nodiscard]] inline std::span<const std::uint16_t> storage_condition_string_refs(
    const StorageConditionProgramEntry& program) noexcept {
    const auto begin = static_cast<std::size_t>(program.arg0);
    const auto size = static_cast<std::size_t>(program.arg1);
    if (size == 0 || begin >= kStorageConditionStringRefRegistry.size() ||
        size > (kStorageConditionStringRefRegistry.size() - begin)) {
        return {};
    }
    return std::span<const std::uint16_t>{
        kStorageConditionStringRefRegistry.data() + begin, size};
}

[[nodiscard]] inline std::span<const std::uint32_t> storage_condition_tag_values(
    const StorageConditionProgramEntry& program) noexcept {
    const auto begin = static_cast<std::size_t>(program.arg0);
    const auto size = static_cast<std::size_t>(program.arg1);
    if (size == 0 || begin >= kStorageConditionTagRegistry.size() ||
        size > (kStorageConditionTagRegistry.size() - begin)) {
        return {};
    }
    return std::span<const std::uint32_t>{
        kStorageConditionTagRegistry.data() + begin, size};
}

[[nodiscard]] inline const StoragePathNodeEntry* find_storage_path_node_entry(
    std::uint16_t node_index) noexcept {
    return node_index < kStoragePathNodeRegistry.size() ? &kStoragePathNodeRegistry[node_index]
                                                        : nullptr;
}

[[nodiscard]] inline std::span<const StoragePathEdgeEntry> storage_path_edges(
    const StoragePathNodeEntry& node) noexcept {
    const auto begin = static_cast<std::size_t>(node.first_edge_index);
    const auto size = static_cast<std::size_t>(node.edge_count);
    if (size == 0 || begin >= kStoragePathEdgeRegistry.size() ||
        size > (kStoragePathEdgeRegistry.size() - begin)) {
        return {};
    }
    return std::span<const StoragePathEdgeEntry>{kStoragePathEdgeRegistry.data() + begin, size};
}

[[nodiscard]] inline std::span<const std::uint32_t> storage_path_terminal_rule_indices(
    const StoragePathNodeEntry& node) noexcept {
    const auto begin = static_cast<std::size_t>(node.first_rule_index);
    const auto size = static_cast<std::size_t>(node.rule_count);
    if (size == 0 || begin >= kStoragePathTerminalRuleIndexRegistry.size() ||
        size > (kStoragePathTerminalRuleIndexRegistry.size() - begin)) {
        return {};
    }
    return std::span<const std::uint32_t>{
        kStoragePathTerminalRuleIndexRegistry.data() + begin, size};
}

[[nodiscard]] inline std::uint16_t find_storage_path_child_node(
    std::uint16_t node_index,
    std::uint32_t tag_value) noexcept {
    const auto* node = find_storage_path_node_entry(node_index);
    if (node == nullptr) {
        return kInvalidStoragePathNodeIndex;
    }
    for (const auto& edge : storage_path_edges(*node)) {
        if (edge.tag_value == tag_value) {
            return edge.next_node_index;
        }
    }
    return kInvalidStoragePathNodeIndex;
}

[[nodiscard]] inline std::uint16_t find_storage_path_node(
    std::span<const std::uint32_t> path_tags) noexcept {
    std::uint16_t node_index = 0;
    for (const auto tag_value : path_tags) {
        node_index = find_storage_path_child_node(node_index, tag_value);
        if (node_index == kInvalidStoragePathNodeIndex) {
            return node_index;
        }
    }
    return node_index;
}

[[nodiscard]] inline bool storage_path_node_contains_tag(
    std::uint16_t node_index,
    std::uint32_t tag_value) noexcept {
    while (node_index != kInvalidStoragePathNodeIndex) {
        const auto* node = find_storage_path_node_entry(node_index);
        if (node == nullptr) {
            return false;
        }
        if (node->incoming_tag == tag_value) {
            return true;
        }
        node_index = node->parent_node_index;
    }
    return false;
}

[[nodiscard]] inline bool storage_path_node_is_descendant_or_same(
    std::uint16_t node_index,
    std::uint16_t ancestor_index) noexcept {
    while (node_index != kInvalidStoragePathNodeIndex) {
        if (node_index == ancestor_index) {
            return true;
        }
        const auto* node = find_storage_path_node_entry(node_index);
        if (node == nullptr) {
            return false;
        }
        node_index = node->parent_node_index;
    }
    return false;
}

[[nodiscard]] inline std::optional<std::vector<std::uint32_t>> parse_path_signature(
    std::string_view path_signature) {
    std::size_t begin_trim = 0;
    while (begin_trim < path_signature.size() &&
           (path_signature[begin_trim] == ' ' || path_signature[begin_trim] == '\t' ||
            path_signature[begin_trim] == '\r' || path_signature[begin_trim] == '\n')) {
        ++begin_trim;
    }
    path_signature.remove_prefix(begin_trim);
    while (!path_signature.empty() &&
           (path_signature.back() == ' ' || path_signature.back() == '\t' ||
            path_signature.back() == '\r' || path_signature.back() == '\n')) {
        path_signature.remove_suffix(1);
    }
    if (path_signature.empty()) {
        return std::vector<std::uint32_t>{};
    }

    std::vector<std::uint32_t> tags;
    std::size_t begin = 0;
    while (begin < path_signature.size()) {
        const auto end = path_signature.find('/', begin);
        auto token = path_signature.substr(
            begin,
            end == std::string_view::npos ? std::string_view::npos : end - begin);
        while (!token.empty() &&
               (token.front() == ' ' || token.front() == '\t' || token.front() == '\r' ||
                token.front() == '\n')) {
            token.remove_prefix(1);
        }
        while (!token.empty() &&
               (token.back() == ' ' || token.back() == '\t' || token.back() == '\r' ||
                token.back() == '\n')) {
            token.remove_suffix(1);
        }
        if (token.size() != 8) {
            return std::nullopt;
        }
        std::uint32_t value = 0;
        for (const char ch : token) {
            value <<= 4;
            if (ch >= '0' && ch <= '9') {
                value |= static_cast<std::uint32_t>(ch - '0');
            } else if (ch >= 'A' && ch <= 'F') {
                value |= static_cast<std::uint32_t>(ch - 'A' + 10);
            } else if (ch >= 'a' && ch <= 'f') {
                value |= static_cast<std::uint32_t>(ch - 'a' + 10);
            } else {
                return std::nullopt;
            }
        }
        tags.push_back(value);
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }
    return tags;
}

[[nodiscard]] inline std::string format_path_signature(
    std::uint16_t node_index) {
    if (node_index == kInvalidStoragePathNodeIndex) {
        return {};
    }
    std::vector<std::uint32_t> path_tags;
    while (node_index != 0 && node_index != kInvalidStoragePathNodeIndex) {
        const auto* node = find_storage_path_node_entry(node_index);
        if (node == nullptr) {
            return {};
        }
        path_tags.push_back(node->incoming_tag);
        node_index = node->parent_node_index;
    }
    if (path_tags.empty()) {
        return {};
    }
    std::reverse(path_tags.begin(), path_tags.end());
    constexpr char kHex[] = "0123456789ABCDEF";
    std::string text;
    text.resize(path_tags.size() * 8 + (path_tags.size() - 1));
    std::size_t out = 0;
    for (std::size_t index = 0; index < path_tags.size(); ++index) {
        if (index != 0) {
            text[out++] = '/';
        }
        const auto value = path_tags[index];
        for (int shift = 28; shift >= 0; shift -= 4) {
            text[out++] = kHex[(value >> shift) & 0xFu];
        }
    }
    return text;
}

[[nodiscard]] inline bool path_starts_with_signature_text(
    std::uint16_t node_index,
    std::string_view prefix_text) {
    if (prefix_text.empty()) {
        return true;
    }
    const auto prefix_tags = parse_path_signature(prefix_text);
    if (!prefix_tags) {
        return false;
    }
    const auto prefix_node_index = find_storage_path_node(*prefix_tags);
    return prefix_node_index != kInvalidStoragePathNodeIndex &&
           storage_path_node_is_descendant_or_same(node_index, prefix_node_index);
}

[[nodiscard]] inline const KeyRangeEntry* find_key_range(
    std::span<const KeyRangeEntry> ranges,
    std::string_view key) noexcept {
    for (const auto& range : ranges) {
        if (range.key == key) {
            return &range;
        }
    }
    return nullptr;
}

[[nodiscard]] inline std::span<const StorageComponentRegistryEntry> find_storage_components(
    std::string_view iod_xml_id) noexcept {
    if (const auto* range = find_key_range(std::span<const KeyRangeEntry>{kStorageComponentRanges},
                                           iod_xml_id)) {
        return std::span<const StorageComponentRegistryEntry>{
            kStorageComponentRegistry.data() + range->begin,
            static_cast<std::size_t>(range->end - range->begin),
        };
    }
    return {};
}

[[nodiscard]] inline const StorageContextEntry* find_storage_context_entry(
    std::uint16_t context_index) noexcept {
    return context_index < kStorageContextRegistry.size() ? &kStorageContextRegistry[context_index]
                                                          : nullptr;
}

[[nodiscard]] inline std::span<const StorageContextEntry> find_storage_contexts(
    std::string_view component_section_id) noexcept {
    if (const auto* range = find_key_range(
            std::span<const KeyRangeEntry>{kStorageContextRanges},
            component_section_id)) {
        return std::span<const StorageContextEntry>{
            kStorageContextRegistry.data() + range->begin,
            static_cast<std::size_t>(range->end - range->begin),
        };
    }
    return {};
}

[[nodiscard]] inline const StorageContextEntry* find_storage_context(
    std::string_view component_section_id,
    std::string_view context_key) noexcept {
    for (const auto& context : find_storage_contexts(component_section_id)) {
        if (context.context_key() == context_key) {
            return &context;
        }
    }
    return nullptr;
}

[[nodiscard]] inline std::span<const StorageContextTransitionEntry> storage_context_transitions(
    const StorageContextEntry& context) noexcept {
    const auto begin = static_cast<std::size_t>(context.first_transition_index);
    const auto size = static_cast<std::size_t>(context.transition_count);
    if (size == 0 || begin >= kStorageContextTransitionRegistry.size() ||
        size > (kStorageContextTransitionRegistry.size() - begin)) {
        return {};
    }
    return std::span<const StorageContextTransitionEntry>{
        kStorageContextTransitionRegistry.data() + begin, size};
}

[[nodiscard]] inline std::span<const std::uint32_t> storage_context_rule_indices(
    const StorageContextEntry& context) noexcept {
    const auto begin = static_cast<std::size_t>(context.first_rule_index);
    const auto size = static_cast<std::size_t>(context.rule_count);
    if (size == 0 || begin >= kStorageContextRuleIndexRegistry.size() ||
        size > (kStorageContextRuleIndexRegistry.size() - begin)) {
        return {};
    }
    return std::span<const std::uint32_t>{
        kStorageContextRuleIndexRegistry.data() + begin, size};
}

[[nodiscard]] inline std::vector<std::uint16_t> find_root_storage_context_indices(
    std::string_view component_section_id) {
    std::vector<std::uint16_t> indices;
    const auto contexts = find_storage_contexts(component_section_id);
    indices.reserve(contexts.size());
    for (const auto& context : contexts) {
        if (context.is_root) {
            indices.push_back(
                static_cast<std::uint16_t>(&context - kStorageContextRegistry.data()));
        }
    }
    return indices;
}

struct StorageTraversalContext {
    std::vector<std::uint32_t> path_stack;
    std::vector<std::uint16_t> active_context_indices;
    std::vector<std::vector<std::uint16_t>> active_context_stack;
};

inline void append_unique_storage_context_index(
    std::vector<std::uint16_t>& context_indices,
    std::uint16_t context_index) {
    if (context_index == kInvalidStorageContextIndex) {
        return;
    }
    if (std::find(context_indices.begin(), context_indices.end(), context_index) ==
        context_indices.end()) {
        context_indices.push_back(context_index);
    }
}

[[nodiscard]] inline std::vector<std::uint16_t> expand_storage_context_closure(
    std::vector<std::uint16_t> seed_context_indices) {
    for (std::size_t index = 0; index < seed_context_indices.size(); ++index) {
        const auto* context = find_storage_context_entry(seed_context_indices[index]);
        if (context == nullptr) {
            continue;
        }
        for (const auto& transition : storage_context_transitions(*context)) {
            if (transition.sequence_tag_value != 0) {
                continue;
            }
            append_unique_storage_context_index(
                seed_context_indices, transition.next_context_index);
        }
    }
    return seed_context_indices;
}

[[nodiscard]] inline StorageTraversalContext make_storage_traversal_context(
    std::string_view component_section_id) {
    StorageTraversalContext traversal;
    traversal.active_context_indices = expand_storage_context_closure(
        find_root_storage_context_indices(component_section_id));
    return traversal;
}

[[nodiscard]] inline std::span<const std::uint16_t> active_storage_context_indices(
    const StorageTraversalContext& traversal) noexcept {
    return std::span<const std::uint16_t>{traversal.active_context_indices};
}

[[nodiscard]] inline std::span<const std::uint32_t> storage_traversal_path(
    const StorageTraversalContext& traversal) noexcept {
    return std::span<const std::uint32_t>{traversal.path_stack};
}

[[nodiscard]] inline bool enter_storage_sequence_item(
    StorageTraversalContext& traversal,
    std::uint32_t sequence_tag_value) {
    std::vector<std::uint16_t> next_context_indices;

    for (const auto context_index : traversal.active_context_indices) {
        const auto* context = find_storage_context_entry(context_index);
        if (context == nullptr) {
            continue;
        }
        for (const auto& transition : storage_context_transitions(*context)) {
            if (transition.sequence_tag_value != sequence_tag_value) {
                continue;
            }
            append_unique_storage_context_index(
                next_context_indices, transition.next_context_index);
        }
    }

    if (next_context_indices.empty()) {
        return false;
    }

    traversal.active_context_stack.push_back(traversal.active_context_indices);
    traversal.active_context_indices =
        expand_storage_context_closure(std::move(next_context_indices));
    if (sequence_tag_value != 0) {
        traversal.path_stack.push_back(sequence_tag_value);
    }
    return true;
}

inline void leave_storage_sequence_item(StorageTraversalContext& traversal) {
    if (traversal.active_context_stack.empty()) {
        return;
    }
    traversal.active_context_indices = std::move(traversal.active_context_stack.back());
    traversal.active_context_stack.pop_back();
    if (!traversal.path_stack.empty()) {
        traversal.path_stack.pop_back();
    }
}

[[nodiscard]] inline std::vector<std::uint32_t> find_active_storage_context_rule_indices(
    const StorageTraversalContext& traversal,
    std::optional<std::uint32_t> tag_value = std::nullopt) {
    std::vector<std::uint32_t> rule_indices;
    for (const auto context_index : traversal.active_context_indices) {
        const auto* context = find_storage_context_entry(context_index);
        if (context == nullptr) {
            continue;
        }
        for (const auto rule_index : storage_context_rule_indices(*context)) {
            if (rule_index >= kComponentAttributeRuleRegistry.size()) {
                continue;
            }
            if (tag_value.has_value() &&
                kComponentAttributeRuleRegistry[rule_index].tag_value != *tag_value) {
                continue;
            }
            rule_indices.push_back(rule_index);
        }
    }
    return rule_indices;
}

[[nodiscard]] inline std::span<const ComponentAttributeRuleEntry> find_component_attribute_rules(
    std::string_view component_section_id) noexcept {
    if (const auto* range = find_key_range(
            std::span<const KeyRangeEntry>{kComponentAttributeRuleRanges},
            component_section_id)) {
        return std::span<const ComponentAttributeRuleEntry>{
            kComponentAttributeRuleRegistry.data() + range->begin,
            static_cast<std::size_t>(range->end - range->begin),
        };
    }
    return {};
}

[[nodiscard]] inline std::span<const StorageAttributeOverrideEntry> find_storage_attribute_overrides(
    std::string_view iod_xml_id) noexcept {
    if (const auto* range = find_key_range(
            std::span<const KeyRangeEntry>{kStorageAttributeOverrideRanges},
            iod_xml_id)) {
        return std::span<const StorageAttributeOverrideEntry>{
            kStorageAttributeOverrideRegistry.data() + range->begin,
            static_cast<std::size_t>(range->end - range->begin),
        };
    }
    return {};
}

} // namespace dicom::storage
