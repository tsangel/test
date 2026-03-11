#include "charset/charset_detail.hpp"
#include "charset/text_validation.hpp"
#include "charset/generated/jisx0208_tables.hpp"
#include "charset/generated/jisx0212_tables.hpp"
#include "charset/generated/ksx1001_tables.hpp"

#include "diagnostics.h"

#include <algorithm>
#include <bit>
#include <fmt/format.h>

namespace dicom::charset::detail {

bool is_supported_passthrough_charset(SpecificCharacterSet charset) noexcept {
	switch (charset) {
	case SpecificCharacterSet::NONE:
	case SpecificCharacterSet::ISO_IR_6:
	case SpecificCharacterSet::ISO_IR_192:
		return true;
	default:
		return false;
	}
}

void set_error(std::string* out_error, std::string message) {
	if (out_error) {
		*out_error = std::move(message);
	}
}

const SpecificCharacterSetInfo* charset_info_or_null(SpecificCharacterSet charset) noexcept {
	return specific_character_set_info(charset);
}

SpecificCharacterSet base_character_set_for_charset(SpecificCharacterSet charset) noexcept {
	const auto* info = charset_info_or_null(charset);
	if (!info || info->base_character_set == SpecificCharacterSet::Unknown) {
		return charset;
	}
	return info->base_character_set;
}

bool is_supported_single_byte_charset(SpecificCharacterSet charset) noexcept {
	switch (charset) {
	case SpecificCharacterSet::ISO_IR_6:
	case SpecificCharacterSet::ISO_IR_100:
	case SpecificCharacterSet::ISO_IR_101:
	case SpecificCharacterSet::ISO_IR_109:
	case SpecificCharacterSet::ISO_IR_110:
	case SpecificCharacterSet::ISO_IR_144:
	case SpecificCharacterSet::ISO_IR_127:
	case SpecificCharacterSet::ISO_IR_126:
	case SpecificCharacterSet::ISO_IR_138:
	case SpecificCharacterSet::ISO_IR_148:
	case SpecificCharacterSet::ISO_IR_203:
	case SpecificCharacterSet::ISO_IR_13:
	case SpecificCharacterSet::ISO_IR_166:
		return true;
	default:
		return false;
	}
}

bool is_supported_iso_2022_single_byte_charset(SpecificCharacterSet charset) noexcept {
	const auto* info = charset_info_or_null(charset);
	return info && info->uses_iso_2022 &&
	    is_supported_single_byte_charset(base_character_set_for_charset(charset));
}

ParsedSpecificCharacterSet default_specific_character_set_plan() {
	return ParsedSpecificCharacterSet{{SpecificCharacterSet::NONE}, SpecificCharacterSet::NONE};
}

std::optional<ParsedSpecificCharacterSet> parse_charset_terms(
    std::span<const SpecificCharacterSet> terms, std::string_view origin, std::string* out_error) {
	if (terms.empty()) {
		return default_specific_character_set_plan();
	}

	ParsedSpecificCharacterSet parsed{};
	parsed.terms.reserve(terms.size());
	for (const auto term : terms) {
		if (term == SpecificCharacterSet::Unknown) {
			set_error(out_error,
			    fmt::format("CHARSET_UNSUPPORTED origin={} reason=unknown Specific Character Set term",
			        origin));
			return std::nullopt;
		}
		parsed.terms.push_back(term);
	}
	parsed.primary = parsed.terms.front();
	return parsed;
}

std::optional<ParsedSpecificCharacterSet> parse_charset_values(
    std::span<const std::string_view> values, std::string_view tag_string, std::string* out_error) {
	if (values.empty()) {
		return default_specific_character_set_plan();
	}

	ParsedSpecificCharacterSet parsed{};
	parsed.terms.reserve(values.size());
	for (const auto term : values) {
		if (term.empty()) {
			parsed.terms.push_back(SpecificCharacterSet::NONE);
			continue;
		}
		const auto charset = specific_character_set_from_term(term);
		if (charset == SpecificCharacterSet::Unknown) {
			set_error(out_error,
			    fmt::format("CHARSET_UNSUPPORTED tag={} term={} reason=unknown Specific Character Set term",
			        tag_string, term));
			return std::nullopt;
		}
		parsed.terms.push_back(charset);
	}
	if (parsed.terms.empty()) {
		return default_specific_character_set_plan();
	}
	parsed.primary = parsed.terms.front();
	return parsed;
}

std::optional<ParsedSpecificCharacterSet> parse_charset_element(
    const DataElement& element, std::string* out_error) {
	const auto values = element.to_string_views();
	if (!values) {
		return default_specific_character_set_plan();
	}
	return parse_charset_values(*values, element.tag().to_string(), out_error);
}

std::optional<ParsedSpecificCharacterSet> parse_dataset_charset(
    const DataSet& dataset, std::string* out_error) {
	const DataElement& element = dataset[kSpecificCharacterSetTag];
	if (element.is_missing()) {
		return default_specific_character_set_plan();
	}
	return parse_charset_element(element, out_error);
}

bool same_charset_terms(
    const ParsedSpecificCharacterSet& lhs, const ParsedSpecificCharacterSet& rhs) noexcept {
	return lhs.terms == rhs.terms;
}

bool are_equivalent_passthrough_charsets(
    SpecificCharacterSet lhs, SpecificCharacterSet rhs) noexcept {
	if (lhs == rhs) {
		return true;
	}
	const auto lhs_is_default =
	    lhs == SpecificCharacterSet::NONE || lhs == SpecificCharacterSet::ISO_IR_6;
	const auto rhs_is_default =
	    rhs == SpecificCharacterSet::NONE || rhs == SpecificCharacterSet::ISO_IR_6;
	return lhs_is_default && rhs_is_default;
}

bool append_utf8_codepoint(std::string& out, std::uint32_t codepoint) noexcept {
	if (codepoint <= 0x7Fu) {
		out.push_back(static_cast<char>(codepoint));
		return true;
	}
	if (codepoint <= 0x7FFu) {
		out.push_back(static_cast<char>(0xC0u | (codepoint >> 6u)));
		out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
		return true;
	}
	if (codepoint <= 0xFFFFu) {
		if (codepoint >= 0xD800u && codepoint <= 0xDFFFu) {
			return false;
		}
		out.push_back(static_cast<char>(0xE0u | (codepoint >> 12u)));
		out.push_back(static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3Fu)));
		out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
		return true;
	}
	if (codepoint <= 0x10FFFFu) {
		out.push_back(static_cast<char>(0xF0u | (codepoint >> 18u)));
		out.push_back(static_cast<char>(0x80u | ((codepoint >> 12u) & 0x3Fu)));
		out.push_back(static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3Fu)));
		out.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
		return true;
	}
	return false;
}

bool decode_utf8_codepoint(
    std::string_view value, std::size_t& offset, std::uint32_t& codepoint) noexcept {
	const auto* bytes = reinterpret_cast<const std::uint8_t*>(value.data());
	if (offset >= value.size()) {
		return false;
	}
	const auto lead = bytes[offset];
	if (lead <= 0x7Fu) {
		codepoint = lead;
		++offset;
		return true;
	}
	if ((lead & 0xE0u) == 0xC0u) {
		if (offset + 1 >= value.size()) {
			return false;
		}
		const auto b1 = bytes[offset + 1];
		if ((b1 & 0xC0u) != 0x80u) {
			return false;
		}
		codepoint = ((lead & 0x1Fu) << 6u) | (b1 & 0x3Fu);
		if (codepoint < 0x80u) {
			return false;
		}
		offset += 2;
		return true;
	}
	if ((lead & 0xF0u) == 0xE0u) {
		if (offset + 2 >= value.size()) {
			return false;
		}
		const auto b1 = bytes[offset + 1];
		const auto b2 = bytes[offset + 2];
		if ((b1 & 0xC0u) != 0x80u || (b2 & 0xC0u) != 0x80u) {
			return false;
		}
		codepoint =
		    ((lead & 0x0Fu) << 12u) | ((b1 & 0x3Fu) << 6u) | (b2 & 0x3Fu);
		if (codepoint < 0x800u || (codepoint >= 0xD800u && codepoint <= 0xDFFFu)) {
			return false;
		}
		offset += 3;
		return true;
	}
	if ((lead & 0xF8u) == 0xF0u) {
		if (offset + 3 >= value.size()) {
			return false;
		}
		const auto b1 = bytes[offset + 1];
		const auto b2 = bytes[offset + 2];
		const auto b3 = bytes[offset + 3];
		if ((b1 & 0xC0u) != 0x80u || (b2 & 0xC0u) != 0x80u ||
		    (b3 & 0xC0u) != 0x80u) {
			return false;
		}
		codepoint = ((lead & 0x07u) << 18u) | ((b1 & 0x3Fu) << 12u) |
		    ((b2 & 0x3Fu) << 6u) | (b3 & 0x3Fu);
		if (codepoint < 0x10000u || codepoint > 0x10FFFFu) {
			return false;
		}
		offset += 4;
		return true;
	}
	return false;
}

void append_unicode_escape_replacement(std::string& out, std::uint32_t codepoint) {
	if (codepoint <= 0xFFFFu) {
		out.append(fmt::format("(U+{:04X})", codepoint));
		return;
	}
	out.append(fmt::format("(U+{:06X})", codepoint));
}

void append_hex_escape_replacement(std::string& out, std::span<const std::uint8_t> bytes) {
	for (const auto byte : bytes) {
		out.append(fmt::format("(0x{:02X})", byte));
	}
}

void append_decode_replacement(
    std::string& out, DecodeReplacementMode mode, std::span<const std::uint8_t> bytes,
    bool* out_replaced) {
	switch (mode) {
	case DecodeReplacementMode::replace_fffd:
		out.append("\xEF\xBF\xBD");
		if (out_replaced) {
			*out_replaced = true;
		}
		return;
	case DecodeReplacementMode::replace_hex_escape:
		append_hex_escape_replacement(out, bytes);
		if (out_replaced) {
			*out_replaced = true;
		}
		return;
	case DecodeReplacementMode::replace_qmark:
		out.push_back('?');
		if (out_replaced) {
			*out_replaced = true;
		}
		return;
	case DecodeReplacementMode::strict:
	default:
		return;
	}
}

std::optional<std::string> utf8_to_ksx1001_string(std::string_view value, std::string* out_error) {
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
		const auto multibyte = unicode_to_ksx1001_multibyte(codepoint);
		if (multibyte == 0) {
			set_error(out_error, "reason=input contains characters outside ISO 2022 IR 149");
			return std::nullopt;
		}
		encoded.push_back(static_cast<char>(multibyte >> 8u));
		encoded.push_back(static_cast<char>(multibyte & 0xFFu));
	}
	return encoded;
}

std::optional<std::string> utf8_to_jisx0208_string(std::string_view value, std::string* out_error) {
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
		const auto multibyte = unicode_to_jisx0208_multibyte(codepoint);
		if (multibyte == 0) {
			set_error(out_error, "reason=input contains characters outside ISO 2022 IR 87");
			return std::nullopt;
		}
		encoded.push_back(static_cast<char>(multibyte >> 8u));
		encoded.push_back(static_cast<char>(multibyte & 0xFFu));
	}
	return encoded;
}

std::optional<std::string> utf8_to_jisx0212_string(std::string_view value, std::string* out_error) {
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
		const auto multibyte = unicode_to_jisx0212_multibyte(codepoint);
		if (multibyte == 0) {
			set_error(out_error, "reason=input contains characters outside ISO 2022 IR 159");
			return std::nullopt;
		}
		encoded.push_back(static_cast<char>(multibyte >> 8u));
		encoded.push_back(static_cast<char>(multibyte & 0xFFu));
	}
	return encoded;
}

std::optional<std::string> encode_utf8_value_for_charset(
    std::string_view value, SpecificCharacterSet target_charset, VR vr, std::string* out_error) {
	if (is_iso2022_charset(target_charset)) {
		auto encoded =
		    encode_utf8_value_for_iso2022_charset(value, target_charset, vr, false, out_error);
		if (!encoded) {
			return std::nullopt;
		}
		return std::move(encoded->bytes);
	}
	switch (target_charset) {
	case SpecificCharacterSet::ISO_2022_IR_87:
		return utf8_to_jisx0208_string(value, out_error);
	case SpecificCharacterSet::ISO_2022_IR_159:
		return utf8_to_jisx0212_string(value, out_error);
	case SpecificCharacterSet::ISO_2022_IR_149:
		return utf8_to_ksx1001_string(value, out_error);
	case SpecificCharacterSet::ISO_2022_IR_58:
		return utf8_to_gb2312_string(value, out_error);
	case SpecificCharacterSet::NONE:
	case SpecificCharacterSet::ISO_IR_6:
		if (!validate_ascii(value)) {
			set_error(out_error, "reason=input contains characters outside the default repertoire");
			return std::nullopt;
		}
		return std::string(value);
	case SpecificCharacterSet::ISO_IR_192:
		if (!validate_utf8(value)) {
			set_error(out_error, "reason=input is not valid UTF-8");
			return std::nullopt;
		}
		return std::string(value);
	case SpecificCharacterSet::ISO_IR_100:
		return utf8_to_latin1_string(value, out_error);
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
		const auto* table = sbcs_g1_table_for_charset(target_charset);
		if (!table) {
			set_error(out_error, "reason=missing single-byte charset table");
			return std::nullopt;
		}
		return utf8_to_sbcs_string(value, *table, out_error);
	}
	case SpecificCharacterSet::ISO_IR_13: {
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
			if (codepoint >= 0xFF61u && codepoint <= 0xFF9Fu) {
				encoded.push_back(static_cast<char>(0xA1u + (codepoint - 0xFF61u)));
				continue;
			}
			set_error(out_error, "reason=input contains characters outside ISO_IR 13");
			return std::nullopt;
		}
		return encoded;
	}
	case SpecificCharacterSet::GB18030:
		return utf8_to_gb18030_string(value, out_error);
	case SpecificCharacterSet::GBK:
		return utf8_to_gbk_string(value, out_error);
	default:
		set_error(out_error,
		    fmt::format("CHARSET_UNSUPPORTED term={} reason=write target charset is not implemented",
		        specific_character_set_info(target_charset)
		            ? specific_character_set_info(target_charset)->defined_term
		            : std::string_view{"unknown"}));
		return std::nullopt;
	}
}

}  // namespace dicom::charset::detail

namespace dicom::charset {

bool validate_ascii(std::string_view value) noexcept {
	for (const unsigned char ch : value) {
		if (ch > 0x7Fu) {
			return false;
		}
	}
	return true;
}

bool validate_utf8(std::string_view value) noexcept {
	const auto* bytes = reinterpret_cast<const std::uint8_t*>(value.data());
	std::size_t i = 0;
	while (i < value.size()) {
		const auto lead = bytes[i];
		if (lead <= 0x7Fu) {
			++i;
			continue;
		}
		if ((lead & 0xE0u) == 0xC0u) {
			if (i + 1 >= value.size()) {
				return false;
			}
			const auto b1 = bytes[i + 1];
			if ((b1 & 0xC0u) != 0x80u) {
				return false;
			}
			const std::uint32_t codepoint =
			    ((lead & 0x1Fu) << 6u) | (b1 & 0x3Fu);
			if (codepoint < 0x80u) {
				return false;
			}
			i += 2;
			continue;
		}
		if ((lead & 0xF0u) == 0xE0u) {
			if (i + 2 >= value.size()) {
				return false;
			}
			const auto b1 = bytes[i + 1];
			const auto b2 = bytes[i + 2];
			if ((b1 & 0xC0u) != 0x80u || (b2 & 0xC0u) != 0x80u) {
				return false;
			}
			const std::uint32_t codepoint =
			    ((lead & 0x0Fu) << 12u) | ((b1 & 0x3Fu) << 6u) | (b2 & 0x3Fu);
			if (codepoint < 0x800u || (codepoint >= 0xD800u && codepoint <= 0xDFFFu)) {
				return false;
			}
			i += 3;
			continue;
		}
		if ((lead & 0xF8u) == 0xF0u) {
			if (i + 3 >= value.size()) {
				return false;
			}
			const auto b1 = bytes[i + 1];
			const auto b2 = bytes[i + 2];
			const auto b3 = bytes[i + 3];
			if ((b1 & 0xC0u) != 0x80u || (b2 & 0xC0u) != 0x80u ||
			    (b3 & 0xC0u) != 0x80u) {
				return false;
			}
			const std::uint32_t codepoint =
			    ((lead & 0x07u) << 18u) | ((b1 & 0x3Fu) << 12u) |
			    ((b2 & 0x3Fu) << 6u) | (b3 & 0x3Fu);
			if (codepoint < 0x10000u || codepoint > 0x10FFFFu) {
				return false;
			}
			i += 4;
			continue;
		}
		return false;
	}
	return true;
}

}  // namespace dicom::charset
