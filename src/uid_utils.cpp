#include "dicom.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstring>
#include <random>
#include <stdexcept>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace dicom::uid {
namespace {

constexpr std::uint64_t kComponentHashSeed = 0x9E3779B97F4A7C15ULL;

std::uint32_t current_process_id() noexcept {
#if defined(_WIN32)
	return static_cast<std::uint32_t>(::_getpid());
#else
	return static_cast<std::uint32_t>(::getpid());
#endif
}

std::uint64_t fallback_nonce_seed() noexcept {
	const auto now_ticks = std::chrono::high_resolution_clock::now().time_since_epoch().count();
	const auto pid = static_cast<std::uint64_t>(current_process_id());
	const auto seed = static_cast<std::uint64_t>(now_ticks) ^ (pid << 32);
	return seed != 0 ? seed : 1ULL;
}

std::uint64_t make_process_nonce() noexcept {
	try {
		std::random_device rd;
		// Keep both calls: some libstdc++ implementations expose only 32 bits per call.
		const std::uint64_t hi = static_cast<std::uint64_t>(rd()) << 32;
		const std::uint64_t lo = static_cast<std::uint64_t>(rd());
		const std::uint64_t nonce = hi ^ lo;
		return nonce != 0 ? nonce : fallback_nonce_seed();
	} catch (...) {
		return fallback_nonce_seed();
	}
}

std::uint64_t splitmix64(std::uint64_t x) noexcept {
	x += kComponentHashSeed;
	x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
	x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
	return x ^ (x >> 31);
}

bool append_char(char* buffer, std::size_t capacity, std::size_t& pos, char ch) noexcept {
	if (pos >= capacity) {
		return false;
	}
	buffer[pos++] = ch;
	return true;
}

template <typename UInt>
bool append_uint(char* buffer, std::size_t capacity, std::size_t& pos, UInt value) noexcept {
	if (pos >= capacity) {
		return false;
	}
	auto* begin = buffer + pos;
	auto* end = buffer + capacity;
	const auto result = std::to_chars(begin, end, value);
	if (result.ec != std::errc{}) {
		return false;
	}
	pos = static_cast<std::size_t>(result.ptr - buffer);
	return true;
}

std::uint32_t divmod_u96_by_10(std::uint64_t& hi, std::uint32_t& lo) noexcept {
	const std::uint32_t words[3] = {
	    static_cast<std::uint32_t>(hi >> 32),
	    static_cast<std::uint32_t>(hi & 0xFFFFFFFFu),
	    lo,
	};
	std::uint32_t qwords[3] = {};
	std::uint64_t rem = 0;
	for (int i = 0; i < 3; ++i) {
		const std::uint64_t cur = (rem << 32) | words[i];
		qwords[i] = static_cast<std::uint32_t>(cur / 10ULL);
		rem = cur % 10ULL;
	}
	hi = (static_cast<std::uint64_t>(qwords[0]) << 32) | qwords[1];
	lo = qwords[2];
	return static_cast<std::uint32_t>(rem);
}

bool append_u96_decimal(char* buffer, std::size_t capacity, std::size_t& pos,
    std::uint64_t hi, std::uint32_t lo) noexcept {
	char digits[29];
	std::size_t n = 0;
	if (hi == 0 && lo == 0) {
		digits[n++] = '0';
	} else {
		while (hi != 0 || lo != 0) {
			const auto rem = divmod_u96_by_10(hi, lo);
			digits[n++] = static_cast<char>('0' + rem);
		}
	}
	if (pos + n > capacity) {
		return false;
	}
	while (n > 0) {
		buffer[pos++] = digits[--n];
	}
	return true;
}

}  // namespace

bool is_valid_uid_text_strict(std::string_view text) noexcept {
	if (text.empty() || text.size() > Generated::max_str_length) {
		return false;
	}
	if (text.front() == '.' || text.back() == '.') {
		return false;
	}
	if (text.front() < '0' || text.front() > '9') {
		return false;
	}

	std::size_t component_start = 0;
	for (std::size_t i = 0; i < text.size(); ++i) {
		const char ch = text[i];
		if (ch == '.') {
			if (i == component_start) {
				return false;  // Empty component.
			}
			const std::size_t component_length = i - component_start;
			if (component_length > 1 && text[component_start] == '0') {
				return false;  // Leading zero in a multi-digit component.
			}
			component_start = i + 1;
			continue;
		}
		if (ch < '0' || ch > '9') {
			return false;
		}
	}

	const std::size_t last_component_length = text.size() - component_start;
	if (last_component_length > 1 && text[component_start] == '0') {
		return false;
	}
	return true;
}

std::optional<Generated> make_uid_with_suffix(std::string_view root, std::uint64_t suffix) noexcept {
	if (!is_valid_uid_text_strict(root)) {
		return std::nullopt;
	}

	Generated uid{};
	constexpr std::size_t kCapacity = Generated::max_str_length;
	if (root.size() + 2 > kCapacity + 1) {
		return std::nullopt;
	}

	std::size_t pos = 0;
	std::memcpy(uid.buffer.data(), root.data(), root.size());
	pos += root.size();
	if (!append_char(uid.buffer.data(), kCapacity, pos, '.')) {
		return std::nullopt;
	}
	if (!append_uint(uid.buffer.data(), kCapacity, pos, suffix)) {
		return std::nullopt;
	}

	uid.buffer[pos] = '\0';
	uid.length = static_cast<Generated::size_type>(pos);
	if (!is_valid_uid_text_strict(uid.value())) {
		return std::nullopt;
	}
	return uid;
}

std::optional<Generated> make_uid_with_suffix(std::uint64_t suffix) noexcept {
	return make_uid_with_suffix(kUidPrefix, suffix);
}

std::optional<Generated> Generated::try_append(std::uint64_t component) const noexcept {
	const auto base = value();
	if (auto direct = make_uid_with_suffix(base, component)) {
		return direct;
	}

	// Fallback: rebuild as "<first 30 chars>[.]<u96-decimal>".
	// This guarantees room under 64 chars while preserving a base prefix.
	constexpr std::size_t kCapacity = Generated::max_str_length;
	if (!is_valid_uid_text_strict(base)) {
		return std::nullopt;
	}
	constexpr std::size_t kFallbackPrefixLen = 30;
	const std::size_t prefix_len = std::min(base.size(), kFallbackPrefixLen);

	Generated uid{};
	std::size_t pos = 0;
	if (prefix_len > 0) {
		std::memcpy(uid.buffer.data(), base.data(), prefix_len);
		pos = prefix_len;
	}
	if (pos == 0) {
		return std::nullopt;
	}
	if (uid.buffer[pos - 1] != '.') {
		if (!append_char(uid.buffer.data(), kCapacity, pos, '.')) {
			return std::nullopt;
		}
	}

	static const std::uint64_t fallback_nonce = make_process_nonce();
	static std::atomic<std::uint64_t> fallback_counter{0};
	const std::uint64_t sequence = fallback_counter.fetch_add(1, std::memory_order_relaxed);

	// Keep component as the primary input, then mix process randomness + sequence.
	std::uint64_t hi = splitmix64(component ^ fallback_nonce ^ splitmix64(sequence));
	for (const char ch : base) {
		hi = splitmix64(hi ^ static_cast<std::uint8_t>(ch));
	}
	hi = splitmix64(hi ^ static_cast<std::uint64_t>(base.size()));
	const std::uint32_t lo = static_cast<std::uint32_t>(
	    splitmix64(hi ^ sequence ^ (component + kComponentHashSeed)));
	if (!append_u96_decimal(uid.buffer.data(), kCapacity, pos, hi, lo)) {
		return std::nullopt;
	}

	uid.buffer[pos] = '\0';
	uid.length = static_cast<Generated::size_type>(pos);
	if (!is_valid_uid_text_strict(uid.value())) {
		return std::nullopt;
	}
	return uid;
}

Generated Generated::append(std::uint64_t component) const {
	auto extended = try_append(component);
	if (!extended) {
		throw std::runtime_error("Failed to append UID component");
	}
	return *extended;
}

std::optional<Generated> try_generate_uid() noexcept {
	static const std::uint64_t process_nonce = make_process_nonce();
	static std::atomic<std::uint64_t> counter{0};

	const std::uint64_t raw_counter = counter.fetch_add(1, std::memory_order_relaxed);
	const std::uint64_t nonce_component = process_nonce ^ (raw_counter >> 32);
	const std::uint32_t sequence = static_cast<std::uint32_t>(raw_counter & 0xFFFFFFFFu);

	Generated uid{};
	constexpr std::size_t kCapacity = Generated::max_str_length;
	std::size_t pos = 0;

	if (kUidPrefix.size() > kCapacity) {
		return std::nullopt;
	}
	std::memcpy(uid.buffer.data(), kUidPrefix.data(), kUidPrefix.size());
	pos += kUidPrefix.size();

	if (!append_char(uid.buffer.data(), kCapacity, pos, '.') ||
	    !append_u96_decimal(uid.buffer.data(), kCapacity, pos, nonce_component, sequence)) {
		return std::nullopt;
	}

	uid.buffer[pos] = '\0';
	uid.length = static_cast<Generated::size_type>(pos);

	if (!is_valid_uid_text_strict(uid.value())) {
		return std::nullopt;
	}
	return uid;
}

Generated generate_uid() {
	auto generated = try_generate_uid();
	if (!generated) {
		throw std::runtime_error("Failed to build generated UID");
	}
	return *generated;
}

Generated generate_sop_instance_uid() {
	return generate_uid();
}

Generated generate_series_instance_uid() {
	return generate_uid();
}

Generated generate_study_instance_uid() {
	return generate_uid();
}

}  // namespace dicom::uid
