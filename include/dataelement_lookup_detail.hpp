// Core dictionary lookup helpers that do not depend on Tag/VR types.
#pragma once

#include <array>
#include <cstring>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>

#include "dataelement_registry.hpp"
#include "dataelement_lookup_tables.hpp"

namespace dicom::lookup {

namespace detail {

constexpr std::uint16_t kInvalidRegistryIndex = std::numeric_limits<std::uint16_t>::max();
constexpr std::uint64_t kFnv64Offset = 0xCBF29CE484222325ull;
constexpr std::uint64_t kFnv64Prime = 0x100000001B3ull;

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

inline bool runtime_sv_equal(std::string_view lhs, std::string_view rhs) noexcept {
	return lhs.size() == rhs.size() &&
	    std::memcmp(lhs.data(), rhs.data(), lhs.size()) == 0;
}

struct RuntimeKeywordCacheEntry {
	std::uint64_t hash{0};
	const DataElementEntry* entry{nullptr};
};

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
	std::uint64_t value = kFnv64Offset;
	for (const unsigned char ch : text) {
		value ^= static_cast<std::uint64_t>(ch);
		value = (value * kFnv64Prime) & 0xFFFFFFFFFFFFFFFFull;
	}
	return value;
}

constexpr std::uint64_t keyword_hash64(std::string_view text, std::uint32_t seed) {
	const auto seed_mix = static_cast<std::uint64_t>(seed) * 0x9E3779B1u;
	return mix64(base_hash64(text) ^ seed_mix);
}

inline std::uint64_t runtime_mix64(std::uint64_t value) noexcept {
	value ^= value >> 30;
	value *= 0xBF58476D1CE4E5B9ull;
	value ^= value >> 27;
	value *= 0x94D049BB133111EBull;
	value ^= value >> 31;
	return value;
}

inline std::uint64_t runtime_base_hash64(std::string_view text) noexcept {
	std::uint64_t value = kFnv64Offset;
	for (const unsigned char ch : text) {
		value ^= static_cast<std::uint64_t>(ch);
		value = (value * kFnv64Prime) & 0xFFFFFFFFFFFFFFFFull;
	}
	return value;
}

inline std::uint64_t runtime_keyword_hash64(
    std::string_view text, std::uint32_t seed) noexcept {
	const auto seed_mix = static_cast<std::uint64_t>(seed) * 0x9E3779B1u;
	return runtime_mix64(runtime_base_hash64(text) ^ seed_mix);
}

constexpr std::uint64_t tag_hash64(std::uint32_t tag_value, std::uint32_t seed) {
	const auto seed_mix = static_cast<std::uint64_t>(seed) * 0x9E3779B1u;
	return mix64(static_cast<std::uint64_t>(tag_value) ^ seed_mix);
}

constexpr auto build_registry_tag_values() {
	std::array<std::uint32_t, kDataElementRegistry.size()> values{};
	for (std::size_t i = 0; i < values.size(); ++i) {
		values[i] = kDataElementRegistry[i].tag_value;
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

inline std::uint16_t keyword_to_registry_index_runtime_with_hash(
    std::string_view keyword, std::uint64_t hash) noexcept {
	const auto bucket = static_cast<std::size_t>(hash % kPerfectHashBucketCount);
	const auto displacement = kPerfectHashDisplacements[bucket];
	const auto slot = static_cast<std::size_t>((hash + displacement) & kPerfectHashMask);
	const auto index = kPerfectHashSlots[slot];
	if (index == detail::kInvalidRegistryIndex) {
		return detail::kInvalidRegistryIndex;
	}
	const auto* entry = detail::entry_from_index(index);
	return entry && detail::runtime_sv_equal(entry->keyword, keyword)
	           ? index
	           : detail::kInvalidRegistryIndex;
}

inline std::uint16_t keyword_to_registry_index_runtime_uncached(
    std::string_view keyword) noexcept {
	if (keyword.empty()) {
		return detail::kInvalidRegistryIndex;
	}
	return keyword_to_registry_index_runtime_with_hash(
	    keyword, detail::runtime_keyword_hash64(keyword, kPerfectHashSeed));
}

inline const DataElementEntry* keyword_to_entry_runtime_uncached(
    std::string_view keyword) noexcept {
	const auto index = keyword_to_registry_index_runtime_uncached(keyword);
	return index == detail::kInvalidRegistryIndex ? nullptr : detail::entry_from_index(index);
}

inline const DataElementEntry* keyword_to_entry_runtime_cached(std::string_view keyword) noexcept {
	if (keyword.empty()) {
		return nullptr;
	}

	thread_local std::array<detail::RuntimeKeywordCacheEntry, 4> cache{};
	const auto hash = detail::runtime_keyword_hash64(keyword, kPerfectHashSeed);
	const auto slot_index = static_cast<std::size_t>(hash) & (cache.size() - 1);
	const auto& slot = cache[slot_index];
	if (slot.entry && slot.hash == hash &&
	    detail::runtime_sv_equal(slot.entry->keyword, keyword)) {
		return slot.entry;
	}

	const auto index = keyword_to_registry_index_runtime_with_hash(keyword, hash);
	const auto* entry =
	    index == detail::kInvalidRegistryIndex ? nullptr : detail::entry_from_index(index);
	if (entry) {
		cache[slot_index] = detail::RuntimeKeywordCacheEntry{hash, entry};
	}
	return entry;
}

inline const DataElementEntry* keyword_to_entry_runtime(std::string_view keyword) noexcept {
	return keyword_to_entry_runtime_cached(keyword);
}

constexpr std::string_view keyword_to_tag_chd(std::string_view keyword) {
	if (const auto* entry = keyword_to_entry_chd(keyword)) {
		return entry->tag;
	}
	return {};
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

}  // namespace dicom::lookup
