#include "charset/charset_detail.hpp"

#include "charset/generated/gb18030_tables.hpp"
#include "charset/text_validation.hpp"

#include <bit>
#include <fmt/format.h>

namespace dicom::charset::detail {
namespace {

bool gb18030_bmp_codepoint_to_unicode(std::uint32_t codepoint, std::uint32_t& unicode) noexcept {
	constexpr auto range_count =
	    sizeof(tables::gb18030_codepoint_range) / sizeof(tables::gb18030_codepoint_range[0]) / 2u;
	if (codepoint < tables::gb18030_codepoint_range[0] ||
	    codepoint > tables::gb18030_codepoint_range[range_count * 2u - 1u]) {
		return false;
	}

	std::size_t left = 0;
	std::size_t right = range_count;
	std::size_t mid = 0;
	while (left < right) {
		mid = (left + right) / 2u;
		const auto start = tables::gb18030_codepoint_range[mid * 2u];
		const auto end = tables::gb18030_codepoint_range[mid * 2u + 1u];
		if (codepoint < start) {
			right = mid;
		} else if (codepoint > end) {
			left = mid + 1u;
		} else {
			unicode = tables::gb18030_unicode_range[mid * 2u] + codepoint - start;
			return true;
		}
	}
	return false;
}

bool unicode_to_gb18030_bmp_codepoint(std::uint32_t unicode, std::uint32_t& codepoint) noexcept {
	constexpr auto range_count =
	    sizeof(tables::gb18030_unicode_range) / sizeof(tables::gb18030_unicode_range[0]) / 2u;
	if (unicode < tables::gb18030_unicode_range[0] ||
	    unicode > tables::gb18030_unicode_range[range_count * 2u - 1u]) {
		return false;
	}

	std::size_t left = 0;
	std::size_t right = range_count;
	std::size_t mid = 0;
	while (left < right) {
		mid = (left + right) / 2u;
		const auto start = tables::gb18030_unicode_range[mid * 2u];
		const auto end = tables::gb18030_unicode_range[mid * 2u + 1u];
		if (unicode < start) {
			right = mid;
		} else if (unicode > end) {
			left = mid + 1u;
		} else {
			codepoint = tables::gb18030_codepoint_range[mid * 2u] + unicode - start;
			return true;
		}
	}
	return false;
}

}  // namespace

std::uint16_t unicode_to_gbk_multibyte(std::uint32_t codepoint) noexcept {
	if (codepoint > 0xFFFFu) {
		return 0;
	}
	const auto unicode_value = static_cast<std::uint16_t>(codepoint);
	const auto pageindex = tables::map_gb18030_unicode_to_index[unicode_value >> 8u];
	if (pageindex == 0) {
		return 0;
	}

	auto codeindex = static_cast<std::size_t>(pageindex + ((unicode_value & 0xF0u) >> 3u));
	auto usebit = tables::map_gb18030_unicode_to_index[codeindex + 1];
	codeindex = tables::map_gb18030_unicode_to_index[codeindex];
	usebit >>= (15u - (unicode_value & 0x0Fu));
	if ((usebit & 1u) == 0u) {
		return 0;
	}

	usebit >>= 1u;
	codeindex += std::popcount(static_cast<unsigned int>(usebit));
	return tables::map_gb18030_index_to_multibyte[codeindex];
}

std::optional<std::string> gb2312_to_utf8_string(
    std::string_view value, std::string_view charset_name, std::string* out_error) {
	std::string utf8;
	utf8.reserve(value.size() * 3u);
	std::size_t offset = 0;
	while (offset < value.size()) {
		const auto b1 = static_cast<std::uint8_t>(value[offset]);
		if (b1 < 0x80u) {
			if (!append_utf8_codepoint(utf8, b1)) {
				set_error(out_error, "reason=failed to encode source byte as UTF-8");
				return std::nullopt;
			}
			++offset;
			continue;
		}
		if (offset + 1u >= value.size()) {
			set_error(out_error,
			    fmt::format("reason=truncated {} byte sequence", charset_name));
			return std::nullopt;
		}

		const auto b2 = static_cast<std::uint8_t>(value[offset + 1u]);
		if (b1 < 0xA1u || b1 > 0xFEu || b2 < 0xA1u || b2 > 0xFEu) {
			set_error(out_error,
			    fmt::format("reason=source byte sequence is not valid {}", charset_name));
			return std::nullopt;
		}

		auto index = static_cast<std::size_t>((b1 - 0x81u) * 190u + (b2 - 0x40u));
		--index;
		const auto codepoint = tables::map_gb18030_to_unicode[index];
		if (codepoint == 0xFFFDu) {
			set_error(out_error,
			    fmt::format("reason=source byte sequence is not representable in {}", charset_name));
			return std::nullopt;
		}
		if (!append_utf8_codepoint(utf8, codepoint)) {
			set_error(out_error, "reason=failed to encode source codepoint as UTF-8");
			return std::nullopt;
		}
		offset += 2u;
	}
	return utf8;
}

std::optional<std::string> utf8_to_gb2312_string(std::string_view value, std::string* out_error) {
	if (!validate_utf8(value)) {
		set_error(out_error, "reason=input is not valid UTF-8");
		return std::nullopt;
	}
	std::string encoded;
	encoded.reserve(value.size() * 2u);
	std::size_t offset = 0;
	while (offset < value.size()) {
		std::uint32_t codepoint = 0;
		if (!decode_utf8_codepoint(value, offset, codepoint)) {
			set_error(out_error, "reason=input is not valid UTF-8");
			return std::nullopt;
		}
		if (codepoint < 0x80u) {
			encoded.push_back(static_cast<char>(codepoint));
			continue;
		}
		const auto multibyte = unicode_to_gbk_multibyte(codepoint);
		if (multibyte == 0) {
			set_error(out_error, "reason=input contains characters outside ISO 2022 IR 58");
			return std::nullopt;
		}
		const auto lead = static_cast<std::uint8_t>(multibyte >> 8u);
		const auto trail = static_cast<std::uint8_t>(multibyte & 0xFFu);
		if (lead < 0xA1u || lead > 0xFEu || trail < 0xA1u || trail > 0xFEu) {
			set_error(out_error, "reason=input contains characters outside ISO 2022 IR 58");
			return std::nullopt;
		}
		encoded.push_back(static_cast<char>(lead));
		encoded.push_back(static_cast<char>(trail));
	}
	return encoded;
}

std::optional<std::string> gb18030_to_utf8_string(
    std::string_view value, bool allow_four_byte, std::string_view charset_name,
    DecodeReplacementMode mode, std::string* out_error, bool* out_replaced) {
	std::string utf8;
	utf8.reserve(value.size() * 3u);
	std::size_t offset = 0;
	while (offset < value.size()) {
		const auto b1 = static_cast<std::uint8_t>(value[offset]);
		if (b1 < 0x80u) {
			if (!append_utf8_codepoint(utf8, b1)) {
				set_error(out_error, "reason=failed to encode source byte as UTF-8");
				return std::nullopt;
			}
			++offset;
			continue;
		}

		if (offset + 1u >= value.size()) {
			if (mode != DecodeReplacementMode::strict) {
				append_decode_replacement(
				    utf8, mode, std::span<const std::uint8_t>(
				                    reinterpret_cast<const std::uint8_t*>(value.data()) + offset,
				                    value.size() - offset), out_replaced);
				offset = value.size();
				continue;
			}
			set_error(out_error,
			    fmt::format("reason=truncated {} byte sequence", charset_name));
			return std::nullopt;
		}

		const auto b2 = static_cast<std::uint8_t>(value[offset + 1u]);
		if (b1 >= 0x81u && b1 <= 0xFEu && b2 >= 0x40u && b2 <= 0xFEu && b2 != 0x7Fu) {
			auto index = static_cast<std::size_t>((b1 - 0x81u) * 190u + (b2 - 0x40u));
			if (b2 > 0x7Fu) {
				--index;
			}
			const auto codepoint = tables::map_gb18030_to_unicode[index];
			if (codepoint == 0xFFFDu) {
				if (mode != DecodeReplacementMode::strict) {
					append_decode_replacement(
					    utf8, mode,
					    std::span<const std::uint8_t>(
					        reinterpret_cast<const std::uint8_t*>(value.data()) + offset, 2u), out_replaced);
					offset += 2u;
					continue;
				}
				set_error(out_error,
				    fmt::format("reason=source byte sequence is not representable in {}", charset_name));
				return std::nullopt;
			}
			if (!append_utf8_codepoint(utf8, codepoint)) {
				set_error(out_error, "reason=failed to encode source codepoint as UTF-8");
				return std::nullopt;
			}
			offset += 2u;
			continue;
		}

		if (!allow_four_byte) {
			if (mode != DecodeReplacementMode::strict) {
				append_decode_replacement(
				    utf8, mode,
				    std::span<const std::uint8_t>(
				        reinterpret_cast<const std::uint8_t*>(value.data()) + offset, 2u), out_replaced);
				offset += 2u;
				continue;
			}
			set_error(out_error,
			    fmt::format("reason=source byte sequence is not valid {}", charset_name));
			return std::nullopt;
		}
		if (offset + 3u >= value.size()) {
			if (mode != DecodeReplacementMode::strict) {
				append_decode_replacement(
				    utf8, mode, std::span<const std::uint8_t>(
				                    reinterpret_cast<const std::uint8_t*>(value.data()) + offset,
				                    value.size() - offset), out_replaced);
				offset = value.size();
				continue;
			}
			set_error(out_error,
			    fmt::format("reason=truncated {} byte sequence", charset_name));
			return std::nullopt;
		}
		const auto b3 = static_cast<std::uint8_t>(value[offset + 2u]);
		const auto b4 = static_cast<std::uint8_t>(value[offset + 3u]);
		if (b1 >= 0x81u && b1 <= 0x84u && b2 >= 0x30u && b2 <= 0x39u &&
		    b3 >= 0x81u && b3 <= 0xFEu && b4 >= 0x30u && b4 <= 0x39u) {
			const auto codepoint =
			    (b4 - 0x30u) + (b3 - 0x81u) * 10u + (b2 - 0x30u) * 1260u +
			    (b1 - 0x81u) * 12600u;
			std::uint32_t unicode = 0;
			if (!gb18030_bmp_codepoint_to_unicode(codepoint, unicode)) {
				if (mode != DecodeReplacementMode::strict) {
					append_decode_replacement(
					    utf8, mode,
					    std::span<const std::uint8_t>(
					        reinterpret_cast<const std::uint8_t*>(value.data()) + offset, 4u), out_replaced);
					offset += 4u;
					continue;
				}
				set_error(out_error,
				    fmt::format("reason=source byte sequence is not valid {}", charset_name));
				return std::nullopt;
			}
			if (!append_utf8_codepoint(utf8, unicode)) {
				set_error(out_error, "reason=failed to encode source codepoint as UTF-8");
				return std::nullopt;
			}
			offset += 4u;
			continue;
		}
		if (b1 >= 0x90u && b1 <= 0xE3u && b2 >= 0x30u && b2 <= 0x39u &&
		    b3 >= 0x81u && b3 <= 0xFEu && b4 >= 0x30u && b4 <= 0x39u) {
			const auto unicode =
			    (b4 - 0x30u) + (b3 - 0x81u) * 10u + (b2 - 0x30u) * 1260u +
			    (b1 - 0x90u) * 12600u + 0x10000u;
			if (!append_utf8_codepoint(utf8, unicode)) {
				set_error(out_error, "reason=failed to encode source codepoint as UTF-8");
				return std::nullopt;
			}
			offset += 4u;
			continue;
		}

		if (mode != DecodeReplacementMode::strict) {
			const auto consume =
			    ((b1 >= 0x81u && b1 <= 0x84u) || (b1 >= 0x90u && b1 <= 0xE3u)) ? 4u : 2u;
			append_decode_replacement(
			    utf8, mode,
			    std::span<const std::uint8_t>(
			        reinterpret_cast<const std::uint8_t*>(value.data()) + offset,
			        std::min<std::size_t>(consume, value.size() - offset)), out_replaced);
			offset += std::min<std::size_t>(consume, value.size() - offset);
			continue;
		}
		set_error(out_error,
		    fmt::format("reason=source byte sequence is not valid {}", charset_name));
		return std::nullopt;
	}
	return utf8;
}

std::optional<std::string> utf8_to_gbk_string(std::string_view value, std::string* out_error) {
	if (!validate_utf8(value)) {
		set_error(out_error, "reason=input is not valid UTF-8");
		return std::nullopt;
	}
	std::string encoded;
	encoded.reserve(value.size() * 2u);
	std::size_t offset = 0;
	while (offset < value.size()) {
		std::uint32_t codepoint = 0;
		if (!decode_utf8_codepoint(value, offset, codepoint)) {
			set_error(out_error, "reason=input is not valid UTF-8");
			return std::nullopt;
		}
		if (codepoint < 0x80u) {
			encoded.push_back(static_cast<char>(codepoint));
			continue;
		}
		const auto multibyte = unicode_to_gbk_multibyte(codepoint);
		if (multibyte == 0) {
			set_error(out_error, "reason=input contains characters outside GBK");
			return std::nullopt;
		}
		encoded.push_back(static_cast<char>(multibyte >> 8u));
		encoded.push_back(static_cast<char>(multibyte & 0xFFu));
	}
	return encoded;
}

std::optional<std::string> utf8_to_gb18030_string(std::string_view value, std::string* out_error) {
	if (!validate_utf8(value)) {
		set_error(out_error, "reason=input is not valid UTF-8");
		return std::nullopt;
	}
	std::string encoded;
	encoded.reserve(value.size() * 4u);
	std::size_t offset = 0;
	while (offset < value.size()) {
		std::uint32_t codepoint = 0;
		if (!decode_utf8_codepoint(value, offset, codepoint)) {
			set_error(out_error, "reason=input is not valid UTF-8");
			return std::nullopt;
		}
		if (codepoint < 0x80u) {
			encoded.push_back(static_cast<char>(codepoint));
			continue;
		}
		if (const auto multibyte = unicode_to_gbk_multibyte(codepoint); multibyte != 0) {
			encoded.push_back(static_cast<char>(multibyte >> 8u));
			encoded.push_back(static_cast<char>(multibyte & 0xFFu));
			continue;
		}
		if (codepoint <= 0xFFFFu) {
			std::uint32_t gb18030_codepoint = 0;
			if (!unicode_to_gb18030_bmp_codepoint(codepoint, gb18030_codepoint)) {
				set_error(out_error, "reason=input contains characters outside GB18030");
				return std::nullopt;
			}
			encoded.push_back(static_cast<char>((gb18030_codepoint / 12600u) + 0x81u));
			gb18030_codepoint %= 12600u;
			encoded.push_back(static_cast<char>((gb18030_codepoint / 1260u) + 0x30u));
			gb18030_codepoint %= 1260u;
			encoded.push_back(static_cast<char>((gb18030_codepoint / 10u) + 0x81u));
			encoded.push_back(static_cast<char>((gb18030_codepoint % 10u) + 0x30u));
			continue;
		}
		if (codepoint <= 0x10FFFFu) {
			auto four_byte_codepoint = codepoint - 0x10000u;
			encoded.push_back(static_cast<char>((four_byte_codepoint / 12600u) + 0x90u));
			four_byte_codepoint %= 12600u;
			encoded.push_back(static_cast<char>((four_byte_codepoint / 1260u) + 0x30u));
			four_byte_codepoint %= 1260u;
			encoded.push_back(static_cast<char>((four_byte_codepoint / 10u) + 0x81u));
			encoded.push_back(static_cast<char>((four_byte_codepoint % 10u) + 0x30u));
			continue;
		}
		set_error(out_error, "reason=input contains characters outside GB18030");
		return std::nullopt;
	}
	return encoded;
}

}  // namespace dicom::charset::detail
