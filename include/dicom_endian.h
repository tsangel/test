#ifndef DICOMSDL_DICOM_ENDIAN_H_
#define DICOMSDL_DICOM_ENDIAN_H_

#include <bit>
#include <cstdint>
#include <cstring>
#include <type_traits>

#ifndef __has_builtin
#define __has_builtin(x) 0
#endif

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace dicom {
namespace endian {
namespace detail {

inline constexpr std::uint16_t bswap16(std::uint16_t value) noexcept {
#if defined(_MSC_VER)
	return _byteswap_ushort(value);
#elif defined(__has_builtin)
#if __has_builtin(__builtin_bswap16)
	return __builtin_bswap16(value);
#else
	return static_cast<std::uint16_t>((value >> 8) | (value << 8));
#endif
#elif defined(__GNUC__)
	return __builtin_bswap16(value);
#else
	return static_cast<std::uint16_t>((value >> 8) | (value << 8));
#endif
}

inline constexpr std::uint32_t bswap32(std::uint32_t value) noexcept {
#if defined(_MSC_VER)
	return _byteswap_ulong(value);
#elif defined(__has_builtin)
#if __has_builtin(__builtin_bswap32)
	return __builtin_bswap32(value);
#else
	return ((value & 0x000000FFu) << 24) |
	       ((value & 0x0000FF00u) << 8) |
	       ((value & 0x00FF0000u) >> 8) |
	       ((value & 0xFF000000u) >> 24);
#endif
#elif defined(__GNUC__)
	return __builtin_bswap32(value);
#else
	return ((value & 0x000000FFu) << 24) |
	       ((value & 0x0000FF00u) << 8) |
	       ((value & 0x00FF0000u) >> 8) |
	       ((value & 0xFF000000u) >> 24);
#endif
}

inline constexpr std::uint64_t bswap64(std::uint64_t value) noexcept {
#if defined(_MSC_VER)
	return _byteswap_uint64(value);
#elif defined(__has_builtin)
#if __has_builtin(__builtin_bswap64)
	return __builtin_bswap64(value);
#else
	return ((value & 0x00000000000000FFull) << 56) |
	       ((value & 0x000000000000FF00ull) << 40) |
	       ((value & 0x0000000000FF0000ull) << 24) |
	       ((value & 0x00000000FF000000ull) << 8) |
	       ((value & 0x000000FF00000000ull) >> 8) |
	       ((value & 0x0000FF0000000000ull) >> 24) |
	       ((value & 0x00FF000000000000ull) >> 40) |
	       ((value & 0xFF00000000000000ull) >> 56);
#endif
#elif defined(__GNUC__)
	return __builtin_bswap64(value);
#else
	return ((value & 0x00000000000000FFull) << 56) |
	       ((value & 0x000000000000FF00ull) << 40) |
	       ((value & 0x0000000000FF0000ull) << 24) |
	       ((value & 0x00000000FF000000ull) << 8) |
	       ((value & 0x000000FF00000000ull) >> 8) |
	       ((value & 0x0000FF0000000000ull) >> 24) |
	       ((value & 0x00FF000000000000ull) >> 40) |
	       ((value & 0xFF00000000000000ull) >> 56);
#endif
}

}  // namespace detail

template <typename T>
constexpr T byteswap(T value) noexcept {
	static_assert(std::is_integral_v<T> || std::is_enum_v<T>, "byteswap requires integral or enum type");
	using Unsigned = std::make_unsigned_t<std::remove_cv_t<T>>;
	Unsigned unsigned_value = static_cast<Unsigned>(value);
	if constexpr (sizeof(T) == 1) {
		return value;
	} else if constexpr (sizeof(T) == 2) {
		return static_cast<T>(detail::bswap16(static_cast<std::uint16_t>(unsigned_value)));
	} else if constexpr (sizeof(T) == 4) {
		return static_cast<T>(detail::bswap32(static_cast<std::uint32_t>(unsigned_value)));
	} else if constexpr (sizeof(T) == 8) {
		return static_cast<T>(detail::bswap64(static_cast<std::uint64_t>(unsigned_value)));
	} else {
		static_assert(sizeof(T) <= 8, "byteswap supports up to 64-bit values");
		return value;
	}
}

inline constexpr bool host_is_little_endian() noexcept {
	return std::endian::native == std::endian::little;
}

inline constexpr bool host_is_big_endian() noexcept {
	return std::endian::native == std::endian::big;
}

template <typename T>
inline T load_le(const void* ptr) noexcept {
	static_assert(std::is_trivially_copyable_v<T>, "load_le requires trivially copyable types");
	static_assert(std::is_integral_v<T> || std::is_enum_v<T>, "load_le is intended for integral-like types");
	T value{};
	std::memcpy(&value, ptr, sizeof(T));
	if constexpr (std::endian::native == std::endian::little) {
		return value;
	}
	return byteswap(value);
}

template <typename T>
inline T load_be(const void* ptr) noexcept {
	static_assert(std::is_trivially_copyable_v<T>, "load_be requires trivially copyable types");
	static_assert(std::is_integral_v<T> || std::is_enum_v<T>, "load_be is intended for integral-like types");
	T value{};
	std::memcpy(&value, ptr, sizeof(T));
	if constexpr (std::endian::native == std::endian::big) {
		return value;
	}
	return byteswap(value);
}

template <typename T>
inline T load_value(const void* ptr, bool little_endian_source) noexcept {
	return little_endian_source ? load_le<T>(ptr) : load_be<T>(ptr);
}

template <typename T>
inline void store_le(void* ptr, T value) noexcept {
	static_assert(std::is_trivially_copyable_v<T>, "store_le requires trivially copyable types");
	static_assert(std::is_integral_v<T> || std::is_enum_v<T>, "store_le is intended for integral-like types");
	if constexpr (std::endian::native != std::endian::little) {
		value = byteswap(value);
	}
	std::memcpy(ptr, &value, sizeof(T));
}

template <typename T>
inline void store_be(void* ptr, T value) noexcept {
	static_assert(std::is_trivially_copyable_v<T>, "store_be requires trivially copyable types");
	static_assert(std::is_integral_v<T> || std::is_enum_v<T>, "store_be is intended for integral-like types");
	if constexpr (std::endian::native != std::endian::big) {
		value = byteswap(value);
	}
	std::memcpy(ptr, &value, sizeof(T));
}

template <typename T>
inline void store_value(void* ptr, T value, bool little_endian_target) noexcept {
	if (little_endian_target) {
		store_le(ptr, value);
	} else {
		store_be(ptr, value);
	}
}

}  // namespace endian
}  // namespace dicom

#endif  // DICOMSDL_DICOM_ENDIAN_H_
