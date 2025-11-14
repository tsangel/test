#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include "uid_registry.hpp"
#include "uid_lookup_tables.hpp"

namespace dicom::uid_lookup {

constexpr std::uint16_t kInvalidUidIndex = std::numeric_limits<std::uint16_t>::max();

namespace detail {

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

constexpr const UidEntry* entry_from_index(std::uint16_t index) {
    return index < kUidRegistry.size() ? &kUidRegistry[index] : nullptr;
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

constexpr std::uint64_t hash64(std::string_view text, std::uint32_t seed) {
    const auto seed_mix = static_cast<std::uint64_t>(seed) * 0x9E3779B1u;
    return mix64(base_hash64(text) ^ seed_mix);
}

}  // namespace detail

constexpr std::uint16_t uid_index_from_value(std::string_view value) {
    if (value.empty()) {
        return kInvalidUidIndex;
    }
    const auto hash = detail::hash64(value, kUidValuePerfectHashSeed);
    const auto bucket = static_cast<std::size_t>(hash % kUidValuePerfectHashBucketCount);
    const auto displacement = kUidValuePerfectHashDisplacements[bucket];
    const auto slot = static_cast<std::size_t>((hash + displacement) & kUidValuePerfectHashMask);
    const auto index = kUidValuePerfectHashSlots[slot];
    if (index == kInvalidUidIndex) {
        return kInvalidUidIndex;
    }
    const auto* entry = detail::entry_from_index(index);
    return entry && detail::sv_equal(entry->value, value) ? index : kInvalidUidIndex;
}

constexpr std::uint16_t uid_index_from_keyword(std::string_view keyword) {
    if (keyword.empty()) {
        return kInvalidUidIndex;
    }
    const auto hash = detail::hash64(keyword, kUidKeywordPerfectHashSeed);
    const auto bucket = static_cast<std::size_t>(hash % kUidKeywordPerfectHashBucketCount);
    const auto displacement = kUidKeywordPerfectHashDisplacements[bucket];
    const auto slot = static_cast<std::size_t>((hash + displacement) & kUidKeywordPerfectHashMask);
    const auto index = kUidKeywordPerfectHashSlots[slot];
    if (index == kInvalidUidIndex) {
        return kInvalidUidIndex;
    }
    const auto* entry = detail::entry_from_index(index);
    return entry && detail::sv_equal(entry->keyword, keyword) ? index : kInvalidUidIndex;
}

constexpr std::uint16_t uid_index_from_text(std::string_view text) {
    if (text.empty()) {
        return kInvalidUidIndex;
    }
    if (const auto keyword_index = uid_index_from_keyword(text); keyword_index != kInvalidUidIndex) {
        return keyword_index;
    }
    return uid_index_from_value(text);
}

inline constexpr const UidEntry* entry_from_index(std::uint16_t index) {
    return detail::entry_from_index(index);
}

inline constexpr const UidEntry* entry_from_value(std::string_view value) {
    const auto index = uid_index_from_value(value);
    return index == kInvalidUidIndex ? nullptr : detail::entry_from_index(index);
}

inline constexpr const UidEntry* entry_from_keyword(std::string_view keyword) {
    const auto index = uid_index_from_keyword(keyword);
    return index == kInvalidUidIndex ? nullptr : detail::entry_from_index(index);
}

inline constexpr const UidEntry* entry_from_text(std::string_view text) {
    const auto index = uid_index_from_text(text);
    return index == kInvalidUidIndex ? nullptr : detail::entry_from_index(index);
}

}  // namespace dicom::uid_lookup
