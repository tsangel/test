// Lookup helpers for keyword -> DataElementEntry mappings.
#pragma once

#include <cstdint>
#include <limits>
#include <string_view>

#include "dataelement_registry.hpp"
#include "keyword_lookup_tables.hpp"

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

constexpr int sv_compare(std::string_view lhs, std::string_view rhs) {
    const std::size_t min_size = lhs.size() < rhs.size() ? lhs.size() : rhs.size();
    for (std::size_t i = 0; i < min_size; ++i) {
        if (lhs[i] < rhs[i]) {
            return -1;
        }
        if (lhs[i] > rhs[i]) {
            return 1;
        }
    }
    if (lhs.size() == rhs.size()) {
        return 0;
    }
    return lhs.size() < rhs.size() ? -1 : 1;
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

constexpr std::uint16_t keyword_to_registry_index_chm(std::string_view keyword) {
    if (keyword.empty()) {
        return detail::kInvalidRegistryIndex;
    }
    const auto hash = detail::keyword_hash64(keyword, kChmSeed);
    const auto u = static_cast<std::size_t>(hash & kChmVertexMask);
    const auto v = static_cast<std::size_t>((hash >> kChmHashShift) & kChmVertexMask);
    const auto slot = static_cast<std::size_t>((kChmGValues[u] ^ kChmGValues[v]) & kChmTableMask);
    const auto index = kChmSlots[slot];
    if (index == detail::kInvalidRegistryIndex) {
        return detail::kInvalidRegistryIndex;
    }
    const auto* entry = detail::entry_from_index(index);
    return entry && detail::sv_equal(entry->keyword, keyword) ? index : detail::kInvalidRegistryIndex;
}

constexpr const DataElementEntry* keyword_to_entry_chm(std::string_view keyword) {
    const auto index = keyword_to_registry_index_chm(keyword);
    return index == detail::kInvalidRegistryIndex ? nullptr : detail::entry_from_index(index);
}

constexpr std::uint16_t keyword_to_registry_index_bdz(std::string_view keyword) {
    if (keyword.empty()) {
        return detail::kInvalidRegistryIndex;
    }
    const auto hash = detail::keyword_hash64(keyword, kBdzSeed);
    const auto v0 = static_cast<std::size_t>(hash & kBdzVertexMask);
    const auto v1 = static_cast<std::size_t>((hash >> kBdzHashShift) & kBdzVertexMask);
    const auto v2 = static_cast<std::size_t>((hash >> kBdzHashDoubleShift) & kBdzVertexMask);
    const auto slot = static_cast<std::size_t>(
        (kBdzGValues[v0] + kBdzGValues[v1] + kBdzGValues[v2]) & kBdzTableMask);
    const auto index = kBdzSlots[slot];
    if (index == detail::kInvalidRegistryIndex) {
        return detail::kInvalidRegistryIndex;
    }
    const auto* entry = detail::entry_from_index(index);
    return entry && detail::sv_equal(entry->keyword, keyword) ? index : detail::kInvalidRegistryIndex;
}

constexpr const DataElementEntry* keyword_to_entry_bdz(std::string_view keyword) {
    const auto index = keyword_to_registry_index_bdz(keyword);
    return index == detail::kInvalidRegistryIndex ? nullptr : detail::entry_from_index(index);
}

constexpr const DataElementEntry* keyword_to_entry_binary(std::string_view keyword) {
    if (keyword.empty()) {
        return nullptr;
    }
    std::size_t left = 0;
    std::size_t right = kKeywordSortedRegistryIndices.size();
    while (left < right) {
        const std::size_t mid = left + (right - left) / 2;
        const auto entry_index = kKeywordSortedRegistryIndices[mid];
        const auto* candidate = detail::entry_from_index(entry_index);
        if (!candidate) {
            return nullptr;
        }
        const int cmp = detail::sv_compare(keyword, candidate->keyword);
        if (cmp == 0) {
            return candidate;
        }
        if (cmp < 0) {
            right = mid;
        } else {
            left = mid + 1;
        }
    }
    return nullptr;
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

constexpr std::string_view keyword_to_tag_chm(std::string_view keyword) {
    if (const auto* entry = keyword_to_entry_chm(keyword)) {
        return entry->tag;
    }
    return {};
}

constexpr std::string_view keyword_to_tag_bdz(std::string_view keyword) {
    if (const auto* entry = keyword_to_entry_bdz(keyword)) {
        return entry->tag;
    }
    return {};
}

constexpr std::string_view keyword_to_tag_perfect(std::string_view keyword) {
    return keyword_to_tag_chd(keyword);
}

constexpr std::string_view keyword_to_tag_binary(std::string_view keyword) {
    if (const auto* entry = keyword_to_entry_binary(keyword)) {
        return entry->tag;
    }
    return {};
}

}  // namespace dicom::lookup
