// Lookup helpers for keyword -> DataElementEntry mappings.
#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <string_view>

#include "dataelement_registry.hpp"
#include "dictionary_lookup_tables.hpp"

namespace dicom::lookup {

namespace detail {

constexpr std::uint16_t kInvalidRegistryIndex = std::numeric_limits<std::uint16_t>::max();

constexpr bool sv_equal(std::string_view lhs, std::string_view rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (lhs[i] != rhs[i]) {
            return false;
        }
    }
    return true;
}

constexpr const DataElementEntry* entry_from_index(std::uint16_t index) {
    return index < kDataElementRegistry.size() ? &kDataElementRegistry[index] : nullptr;
}

constexpr std::uint64_t mix64(std::uint64_t value) {
    value ^= value >> 30;
    value *= 0xBF58476D1CE4E5B9ull;
    value ^= value >> 27;
    value *= 0x94D049BB133111EBull;
    value ^= value >> 31;
    return value;
}

constexpr std::uint64_t base_hash64(std::string_view text) {
    std::uint64_t value = 0x6A09E667F3BCC909ull;
    for (const unsigned char ch : text) {
        value = (value + static_cast<std::uint64_t>(ch) + 0x9E3779B97F4A7C15ull) & 0xFFFFFFFFFFFFFFFFull;
        value = mix64(value);
    }
    return value;
}

constexpr std::uint64_t keyword_hash64(std::string_view text, std::uint32_t seed) {
    const auto seed_mix = static_cast<std::uint64_t>(seed) * 0x9E3779B1u;
    return mix64(base_hash64(text) ^ seed_mix);
}

constexpr std::uint64_t tag_base_hash32(std::uint32_t tag_value) {
    std::uint64_t value = 0x6A09E667F3BCC909ull;
    for (int shift = 0; shift < 32; shift += 8) {
        const auto byte = static_cast<unsigned char>((tag_value >> shift) & 0xFFu);
        value = (value + static_cast<std::uint64_t>(byte) + 0x9E3779B97F4A7C15ull) & 0xFFFFFFFFFFFFFFFFull;
        value = mix64(value);
    }
    return value;
}

constexpr std::uint64_t tag_hash64(std::uint32_t tag_value, std::uint32_t seed) {
    const auto seed_mix = static_cast<std::uint64_t>(seed) * 0x9E3779B1u;
    return mix64(tag_base_hash32(tag_value) ^ seed_mix);
}

constexpr std::uint32_t hex_value(char ch) {
    if (ch >= '0' && ch <= '9') {
        return static_cast<std::uint32_t>(ch - '0');
    }
    if (ch >= 'A' && ch <= 'F') {
        return static_cast<std::uint32_t>(10 + (ch - 'A'));
    }
    if (ch >= 'a' && ch <= 'f') {
        return static_cast<std::uint32_t>(10 + (ch - 'a'));
    }
    return 0;
}

constexpr std::uint32_t tag_string_to_u32(std::string_view tag) {
    std::uint32_t value = 0;
    for (char ch : tag) {
        if (ch == '(' || ch == ')' || ch == ',' || ch == ' ') {
            continue;
        }
        value = (value << 4) | hex_value(ch);
    }
    return value;
}

constexpr auto build_registry_tag_values() {
    std::array<std::uint32_t, kDataElementRegistry.size()> values{};
    for (std::size_t i = 0; i < values.size(); ++i) {
        values[i] = tag_string_to_u32(kDataElementRegistry[i].tag);
    }
    return values;
}

constexpr auto kRegistryTagValues = build_registry_tag_values();

}  // namespace detail

constexpr std::uint16_t keyword_to_registry_index_chd(std::string_view keyword) {
    if (keyword.empty()) {
        return detail::kInvalidRegistryIndex;
    }
    const auto hash = detail::keyword_hash64(keyword, kPerfectHashSeed);
    const auto bucket = static_cast<std::size_t>(hash % kPerfectHashBucketCount);
    const auto displacement = kPerfectHashDisplacements[bucket];
    const auto slot = static_cast<std::size_t>((hash + displacement) & kPerfectHashMask);
    const auto index = kPerfectHashSlots[slot];
    if (index == detail::kInvalidRegistryIndex) {
        return detail::kInvalidRegistryIndex;
    }
    const auto* entry = detail::entry_from_index(index);
    return entry && detail::sv_equal(entry->keyword, keyword) ? index : detail::kInvalidRegistryIndex;
}

constexpr const DataElementEntry* keyword_to_entry_chd(std::string_view keyword) {
    const auto index = keyword_to_registry_index_chd(keyword);
    return index == detail::kInvalidRegistryIndex ? nullptr : detail::entry_from_index(index);
}

constexpr std::uint16_t keyword_to_registry_index_perfect(std::string_view keyword) {
    return keyword_to_registry_index_chd(keyword);
}

constexpr const DataElementEntry* keyword_to_entry_perfect(std::string_view keyword) {
    return keyword_to_entry_chd(keyword);
}

constexpr std::string_view keyword_to_tag_chd(std::string_view keyword) {
    if (const auto* entry = keyword_to_entry_chd(keyword)) {
        return entry->tag;
    }
    return {};
}

constexpr std::string_view keyword_to_tag_perfect(std::string_view keyword) {
    return keyword_to_tag_chd(keyword);
}

constexpr std::uint32_t make_tag(std::uint16_t group, std::uint16_t element) {
    return (static_cast<std::uint32_t>(group) << 16) | static_cast<std::uint32_t>(element);
}

constexpr std::uint16_t tag_to_registry_index_chd(std::uint32_t tag_value) {
    const auto hash = detail::tag_hash64(tag_value, kTagPerfectHashSeed);
    const auto bucket = static_cast<std::size_t>(hash % kTagPerfectHashBucketCount);
    const auto displacement = kTagPerfectHashDisplacements[bucket];
    const auto slot = static_cast<std::size_t>((hash + displacement) & kTagPerfectHashMask);
    const auto index = kTagPerfectHashSlots[slot];
    if (index == detail::kInvalidRegistryIndex) {
        return detail::kInvalidRegistryIndex;
    }
    return detail::kRegistryTagValues[index] == tag_value ? index : detail::kInvalidRegistryIndex;
}

constexpr std::uint16_t tag_to_registry_index_perfect(std::uint32_t tag_value) {
    return tag_to_registry_index_chd(tag_value);
}

constexpr std::uint16_t tag_to_registry_index_wildcard(std::uint32_t tag_value) {
    for (std::size_t i = 0; i < kTagWildcardRegistryIndices.size(); ++i) {
        if ((tag_value & kTagWildcardMasks[i]) == kTagWildcardValues[i]) {
            return kTagWildcardRegistryIndices[i];
        }
    }
    return detail::kInvalidRegistryIndex;
}

constexpr std::uint16_t tag_to_registry_index(std::uint32_t tag_value) {
    const auto exact = tag_to_registry_index_chd(tag_value);
    if (exact != detail::kInvalidRegistryIndex) {
        return exact;
    }
    return tag_to_registry_index_wildcard(tag_value);
}

constexpr const DataElementEntry* tag_to_entry(std::uint32_t tag_value) {
    const auto index = tag_to_registry_index(tag_value);
    return index == detail::kInvalidRegistryIndex ? nullptr : detail::entry_from_index(index);
}

constexpr std::string_view tag_to_keyword(std::uint32_t tag_value) {
    if (const auto* entry = tag_to_entry(tag_value)) {
        return entry->keyword;
    }
    return {};
}

}  // namespace dicom::lookup
