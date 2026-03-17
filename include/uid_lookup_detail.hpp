#pragma once

#include <array>
#include <cstddef>
#include <cstring>
#include <cstdint>
#include <limits>
#include <string_view>
#include <type_traits>

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

constexpr std::uint64_t kFnv64Offset = 0xCBF29CE484222325ull;
constexpr std::uint64_t kFnv64Prime = 0x100000001B3ull;

constexpr std::uint64_t base_hash64(std::string_view text) {
	std::uint64_t value = kFnv64Offset;
	for (const unsigned char ch : text) {
		value ^= static_cast<std::uint64_t>(ch);
		value *= kFnv64Prime;
	}
	return value;
}

constexpr std::uint64_t hash64(std::string_view text, std::uint32_t seed) {
	const auto seed_mix = static_cast<std::uint64_t>(seed) * 0x9E3779B1u;
	return mix64(base_hash64(text) ^ seed_mix);
}

inline bool runtime_sv_equal(std::string_view lhs, std::string_view rhs) noexcept {
	return lhs.size() == rhs.size() &&
	       (lhs.empty() || std::memcmp(lhs.data(), rhs.data(), lhs.size()) == 0);
}

constexpr bool looks_like_uid_value(std::string_view text) noexcept {
	if (text.empty()) {
		return false;
	}
	const char first = text.front();
	if (first < '0' || first > '9') {
		return false;
	}
	for (char ch : text) {
		if ((ch < '0' || ch > '9') && ch != '.') {
			return false;
		}
	}
	return true;
}

inline std::uint64_t runtime_base_hash64(std::string_view text) noexcept {
	std::uint64_t value = kFnv64Offset;
	for (const unsigned char ch : text) {
		value ^= static_cast<std::uint64_t>(ch);
		value *= kFnv64Prime;
	}
	return value;
}

inline std::uint64_t runtime_hash64(std::string_view text, std::uint32_t seed) noexcept {
	const auto seed_mix = static_cast<std::uint64_t>(seed) * 0x9E3779B1u;
	return mix64(runtime_base_hash64(text) ^ seed_mix);
}

struct RuntimeUidCacheEntry {
	static constexpr std::size_t kMaxTextLength = 96;

	std::uint16_t index{kInvalidUidIndex};
	std::uint8_t length{0};
	std::array<char, kMaxTextLength> text{};
};

inline bool runtime_cache_hit(
    const RuntimeUidCacheEntry& entry, std::string_view text) noexcept {
	return entry.index != kInvalidUidIndex && entry.length == text.size() &&
	       (text.empty() || std::memcmp(entry.text.data(), text.data(), text.size()) == 0);
}

inline void runtime_cache_store(
    RuntimeUidCacheEntry& entry, std::string_view text, std::uint16_t index) noexcept {
	entry.index = index;
	if (text.size() > RuntimeUidCacheEntry::kMaxTextLength) {
		entry.length = 0;
		return;
	}
	entry.length = static_cast<std::uint8_t>(text.size());
	if (!text.empty()) {
		std::memcpy(entry.text.data(), text.data(), text.size());
	}
}

inline std::uint16_t uid_index_from_value_runtime_with_hash(
    std::string_view value, std::uint64_t hash) noexcept {
	const auto bucket = static_cast<std::size_t>(hash % kUidValuePerfectHashBucketCount);
	const auto displacement = kUidValuePerfectHashDisplacements[bucket];
	const auto slot = static_cast<std::size_t>((hash + displacement) & kUidValuePerfectHashMask);
	const auto index = kUidValuePerfectHashSlots[slot];
	if (index == kInvalidUidIndex) {
		return kInvalidUidIndex;
	}
	const auto* entry = entry_from_index(index);
	return entry && runtime_sv_equal(entry->value, value) ? index : kInvalidUidIndex;
}

inline std::uint16_t uid_index_from_value_runtime_uncached(std::string_view value) noexcept {
	if (value.empty()) {
		return kInvalidUidIndex;
	}
	return uid_index_from_value_runtime_with_hash(
	    value, runtime_hash64(value, kUidValuePerfectHashSeed));
}

inline std::uint16_t uid_index_from_keyword_runtime_with_hash(
    std::string_view keyword, std::uint64_t hash) noexcept {
	const auto bucket = static_cast<std::size_t>(hash % kUidKeywordPerfectHashBucketCount);
	const auto displacement = kUidKeywordPerfectHashDisplacements[bucket];
	const auto slot = static_cast<std::size_t>((hash + displacement) & kUidKeywordPerfectHashMask);
	const auto index = kUidKeywordPerfectHashSlots[slot];
	if (index == kInvalidUidIndex) {
		return kInvalidUidIndex;
	}
	const auto* entry = entry_from_index(index);
	return entry && runtime_sv_equal(entry->keyword, keyword) ? index : kInvalidUidIndex;
}

inline std::uint16_t uid_index_from_keyword_runtime_uncached(
    std::string_view keyword) noexcept {
	if (keyword.empty()) {
		return kInvalidUidIndex;
	}
	return uid_index_from_keyword_runtime_with_hash(
	    keyword, runtime_hash64(keyword, kUidKeywordPerfectHashSeed));
}

inline std::uint16_t uid_index_from_value_runtime_cached(std::string_view value) noexcept {
	if (value.empty()) {
		return kInvalidUidIndex;
	}
	thread_local std::array<RuntimeUidCacheEntry, 4> cache{};
	thread_local std::size_t next_slot = 0;
	for (const auto& entry : cache) {
		if (runtime_cache_hit(entry, value)) {
			return entry.index;
		}
	}
	const auto index = uid_index_from_value_runtime_uncached(value);
	runtime_cache_store(cache[next_slot], value, index);
	next_slot = (next_slot + 1) % cache.size();
	return index;
}

inline std::uint16_t uid_index_from_keyword_runtime_cached(
    std::string_view keyword) noexcept {
	if (keyword.empty()) {
		return kInvalidUidIndex;
	}
	thread_local std::array<RuntimeUidCacheEntry, 4> cache{};
	thread_local std::size_t next_slot = 0;
	for (const auto& entry : cache) {
		if (runtime_cache_hit(entry, keyword)) {
			return entry.index;
		}
	}
	const auto index = uid_index_from_keyword_runtime_uncached(keyword);
	runtime_cache_store(cache[next_slot], keyword, index);
	next_slot = (next_slot + 1) % cache.size();
	return index;
}

inline std::uint16_t uid_index_from_text_runtime_cached(std::string_view text) noexcept {
	const bool value_like = looks_like_uid_value(text);
	if (value_like) {
		const auto value_index = uid_index_from_value_runtime_cached(text);
		if (value_index != kInvalidUidIndex) {
			return value_index;
		}
	}
	const auto keyword_index = uid_index_from_keyword_runtime_cached(text);
	if (keyword_index != kInvalidUidIndex) {
		return keyword_index;
	}
	return value_like ? kInvalidUidIndex : uid_index_from_value_runtime_cached(text);
}

}  // namespace detail

constexpr std::uint16_t uid_index_from_value_chd(std::string_view value) {
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

constexpr std::uint16_t uid_index_from_keyword_chd(std::string_view keyword) {
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

constexpr std::uint16_t uid_index_from_value(std::string_view value) {
	if (std::is_constant_evaluated()) {
		return uid_index_from_value_chd(value);
	}
	return detail::uid_index_from_value_runtime_cached(value);
}

constexpr std::uint16_t uid_index_from_keyword(std::string_view keyword) {
	if (std::is_constant_evaluated()) {
		return uid_index_from_keyword_chd(keyword);
	}
	return detail::uid_index_from_keyword_runtime_cached(keyword);
}

constexpr std::uint16_t uid_index_from_text(std::string_view text) {
	if (text.empty()) {
		return kInvalidUidIndex;
	}
	if (std::is_constant_evaluated()) {
		const bool value_like = detail::looks_like_uid_value(text);
		if (value_like) {
			const auto value_index = uid_index_from_value_chd(text);
			if (value_index != kInvalidUidIndex) {
				return value_index;
			}
		}
		const auto keyword_index = uid_index_from_keyword_chd(text);
		if (keyword_index != kInvalidUidIndex) {
			return keyword_index;
		}
		return value_like ? kInvalidUidIndex : uid_index_from_value_chd(text);
	}
	return detail::uid_index_from_text_runtime_cached(text);
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
