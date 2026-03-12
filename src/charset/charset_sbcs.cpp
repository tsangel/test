#include "charset/charset_detail.hpp"

#include "charset/generated/sbcs_to_unicode_selected.hpp"
#include "charset/text_validation.hpp"

#include <fmt/format.h>

namespace dicom::charset::detail {
namespace {

std::optional<std::string> sbcs_to_utf8_string(
    std::string_view value, const std::array<std::uint16_t, 128>& g1_table,
    DecodeReplacementMode mode, std::string* out_error, bool* out_replaced) {
	std::string utf8;
	utf8.reserve(value.size() * 3);
	for (const unsigned char ch : value) {
		const std::uint32_t codepoint = ch < 0x80u ? ch : g1_table[ch - 0x80u];
		if (codepoint == 0xFFFDu) {
			if (mode != DecodeReplacementMode::strict) {
				append_decode_replacement(
				    utf8, mode, std::span<const std::uint8_t>(&ch, 1), out_replaced);
				continue;
			}
			set_error(out_error, "reason=source byte is not representable in the declared charset");
			return std::nullopt;
		}
		if (!append_utf8_codepoint(utf8, codepoint)) {
			set_error(out_error, "reason=failed to encode source codepoint as UTF-8");
			return std::nullopt;
		}
	}
	return utf8;
}

}  // namespace

const std::array<std::uint16_t, 128>* sbcs_g1_table_for_charset(
    SpecificCharacterSet charset) noexcept {
	switch (base_character_set_for_charset(charset)) {
	case SpecificCharacterSet::ISO_IR_101:
		return &tables::map_latin2_to_unicode_g1;
	case SpecificCharacterSet::ISO_IR_109:
		return &tables::map_latin3_to_unicode_g1;
	case SpecificCharacterSet::ISO_IR_110:
		return &tables::map_latin4_to_unicode_g1;
	case SpecificCharacterSet::ISO_IR_144:
		return &tables::map_cyrillic_to_unicode_g1;
	case SpecificCharacterSet::ISO_IR_127:
		return &tables::map_arabic_to_unicode_g1;
	case SpecificCharacterSet::ISO_IR_126:
		return &tables::map_greek_to_unicode_g1;
	case SpecificCharacterSet::ISO_IR_138:
		return &tables::map_hebrew_to_unicode_g1;
	case SpecificCharacterSet::ISO_IR_148:
		return &tables::map_latin5_to_unicode_g1;
	case SpecificCharacterSet::ISO_IR_203:
		return &tables::map_latin9_to_unicode_g1;
	case SpecificCharacterSet::ISO_IR_166:
		return &tables::map_thai_to_unicode_g1;
	default:
		return nullptr;
	}
}

std::optional<std::string> latin1_to_utf8_string(std::string_view value, std::string* out_error) {
	std::string utf8;
	utf8.reserve(value.size() * 2);
	for (const unsigned char ch : value) {
		if (!append_utf8_codepoint(utf8, ch)) {
			set_error(out_error, "reason=failed to encode Latin-1 codepoint as UTF-8");
			return std::nullopt;
		}
	}
	return utf8;
}

std::optional<std::string> utf8_to_latin1_string(std::string_view value, std::string* out_error) {
	if (!validate_utf8(value)) {
		set_error(out_error, "reason=input is not valid UTF-8");
		return std::nullopt;
	}
	std::string latin1;
	latin1.reserve(value.size());
	std::size_t offset = 0;
	while (offset < value.size()) {
		std::uint32_t codepoint = 0;
		if (!decode_utf8_codepoint(value, offset, codepoint)) {
			set_error(out_error, "reason=input is not valid UTF-8");
			return std::nullopt;
		}
		if (codepoint > 0xFFu) {
			set_error(out_error, "reason=input contains characters outside ISO_IR 100");
			return std::nullopt;
		}
		latin1.push_back(static_cast<char>(codepoint));
	}
	return latin1;
}

std::optional<std::string> utf8_to_sbcs_string(std::string_view value,
    const std::array<std::uint16_t, 128>& g1_table, std::string* out_error) {
	if (!validate_utf8(value)) {
		set_error(out_error, "reason=input is not valid UTF-8");
		return std::nullopt;
	}
	std::string encoded;
	encoded.reserve(value.size());
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

		bool found = false;
		for (std::size_t i = 0; i < g1_table.size(); ++i) {
			if (g1_table[i] == codepoint) {
				encoded.push_back(static_cast<char>(0x80u + i));
				found = true;
				break;
			}
		}
		if (!found) {
			set_error(out_error,
			    fmt::format("reason=input contains characters outside {}",
			        "the target single-byte repertoire"));
			return std::nullopt;
		}
	}
	return encoded;
}

std::optional<std::string> decode_sbcs_raw_to_utf8(
    std::string_view value, SpecificCharacterSet source_charset, DecodeReplacementMode mode,
    std::string* out_error, bool* out_replaced) {
	switch (base_character_set_for_charset(source_charset)) {
	case SpecificCharacterSet::NONE:
	case SpecificCharacterSet::ISO_IR_6:
		if (mode == DecodeReplacementMode::strict) {
			if (!validate_ascii(value)) {
				set_error(out_error, "reason=element value is not valid for source charset");
				return std::nullopt;
			}
			return std::string(value);
		}
		{
			std::string utf8;
			utf8.reserve(value.size() * 8u);
			for (const auto ch : value) {
				const auto byte = static_cast<std::uint8_t>(ch);
				if (byte < 0x80u) {
					utf8.push_back(static_cast<char>(byte));
					continue;
				}
				append_decode_replacement(
				    utf8, mode, std::span<const std::uint8_t>(&byte, 1), out_replaced);
			}
			return utf8;
		}
	case SpecificCharacterSet::ISO_IR_100:
		return latin1_to_utf8_string(value, out_error);
	case SpecificCharacterSet::ISO_IR_101:
	case SpecificCharacterSet::ISO_IR_109:
	case SpecificCharacterSet::ISO_IR_110:
	case SpecificCharacterSet::ISO_IR_144:
	case SpecificCharacterSet::ISO_IR_127:
	case SpecificCharacterSet::ISO_IR_126:
	case SpecificCharacterSet::ISO_IR_138:
	case SpecificCharacterSet::ISO_IR_148:
	case SpecificCharacterSet::ISO_IR_203:
	case SpecificCharacterSet::ISO_IR_166: {
		const auto* table = sbcs_g1_table_for_charset(source_charset);
		if (!table) {
			set_error(out_error, "reason=missing single-byte charset table");
			return std::nullopt;
		}
		return sbcs_to_utf8_string(value, *table, mode, out_error, out_replaced);
	}
	case SpecificCharacterSet::ISO_IR_13: {
		std::string utf8;
		utf8.reserve(value.size() * 3u);
		for (const auto ch : value) {
			const auto byte = static_cast<std::uint8_t>(ch);
			if (byte < 0x80u) {
				utf8.push_back(static_cast<char>(byte));
				continue;
			}
			if (byte < 0xA1u || byte > 0xDFu) {
				if (mode != DecodeReplacementMode::strict) {
					append_decode_replacement(
					    utf8, mode, std::span<const std::uint8_t>(&byte, 1), out_replaced);
					continue;
				}
				set_error(out_error, "reason=element value is not valid for source charset");
				return std::nullopt;
			}
			if (!append_utf8_codepoint(utf8, 0xFF61u + (byte - 0xA1u))) {
				set_error(out_error, "reason=failed to encode source byte as UTF-8");
				return std::nullopt;
			}
		}
		return utf8;
	}
	default:
		set_error(out_error, "reason=read charset is not implemented");
		return std::nullopt;
	}
}

}  // namespace dicom::charset::detail
