#include "charset/charset_detail.hpp"
#include "charset/text_validation.hpp"
#include "charset/generated/jisx0208_tables.hpp"
#include "charset/generated/jisx0212_tables.hpp"
#include "charset/generated/ksx1001_tables.hpp"

#include <bit>
#include <fmt/format.h>

namespace dicom::charset::detail {

std::uint16_t unicode_to_ksx1001_multibyte(std::uint32_t codepoint) noexcept {
	if (codepoint > 0xFFFFu) {
		return 0;
	}
	const auto unicode_value = static_cast<std::uint16_t>(codepoint);
	const auto pageindex = tables::map_ksx1001_unicode_to_index[unicode_value >> 8u];
	if (pageindex == 0) {
		return 0;
	}

	auto codeindex = static_cast<std::size_t>(pageindex + ((unicode_value & 0xF0u) >> 3u));
	auto usebit = tables::map_ksx1001_unicode_to_index[codeindex + 1];
	codeindex = tables::map_ksx1001_unicode_to_index[codeindex];
	usebit >>= (15u - (unicode_value & 0x0Fu));
	if ((usebit & 1u) == 0u) {
		return 0;
	}

	usebit >>= 1u;
	codeindex += std::popcount(static_cast<unsigned int>(usebit));
	return static_cast<std::uint16_t>(tables::map_ksx1001_index_to_multibyte[codeindex] | 0x8080u);
}

std::uint16_t unicode_to_jisx0208_multibyte(std::uint32_t codepoint) noexcept {
	if (codepoint > 0xFFFFu) {
		return 0;
	}
	const auto unicode_value = static_cast<std::uint16_t>(codepoint);
	const auto pageindex = tables::map_jisx0208_unicode_to_index[unicode_value >> 8u];
	if (pageindex == 0) {
		return 0;
	}

	auto codeindex = static_cast<std::size_t>(pageindex + ((unicode_value & 0xF0u) >> 3u));
	auto usebit = tables::map_jisx0208_unicode_to_index[codeindex + 1];
	codeindex = tables::map_jisx0208_unicode_to_index[codeindex];
	usebit >>= (15u - (unicode_value & 0x0Fu));
	if ((usebit & 1u) == 0u) {
		return 0;
	}

	usebit >>= 1u;
	codeindex += std::popcount(static_cast<unsigned int>(usebit));
	return tables::map_jisx0208_index_to_multibyte[codeindex];
}

std::uint16_t unicode_to_jisx0212_multibyte(std::uint32_t codepoint) noexcept {
	if (codepoint > 0xFFFFu) {
		return 0;
	}
	const auto unicode_value = static_cast<std::uint16_t>(codepoint);
	const auto pageindex = tables::map_jisx0212_unicode_to_index[unicode_value >> 8u];
	if (pageindex == 0) {
		return 0;
	}

	auto codeindex = static_cast<std::size_t>(pageindex + ((unicode_value & 0xF0u) >> 3u));
	auto usebit = tables::map_jisx0212_unicode_to_index[codeindex + 1];
	codeindex = tables::map_jisx0212_unicode_to_index[codeindex];
	usebit >>= (15u - (unicode_value & 0x0Fu));
	if ((usebit & 1u) == 0u) {
		return 0;
	}

	usebit >>= 1u;
	codeindex += std::popcount(static_cast<unsigned int>(usebit));
	return tables::map_jisx0212_index_to_multibyte[codeindex];
}

bool charset_plan_uses_only_iso2022_terms(const ParsedSpecificCharacterSet& parsed) noexcept {
	for (const auto term : parsed.terms) {
		if (term == SpecificCharacterSet::NONE || term == SpecificCharacterSet::ISO_IR_6 ||
		    term == SpecificCharacterSet::ISO_2022_IR_6) {
			continue;
		}
		const auto* info = charset_info_or_null(term);
		if (!info || !info->uses_iso_2022) {
			return false;
		}
	}
	return true;
}

std::optional<SpecificCharacterSet> first_iso2022_g1_term(
    const ParsedSpecificCharacterSet& parsed) noexcept {
	for (const auto term : parsed.terms) {
		const auto* info = charset_info_or_null(term);
		if (!info || !info->uses_iso_2022) {
			continue;
		}
		if (info->code_element == SpecificCharacterSetCodeElement::G1) {
			return term;
		}
	}
	return std::nullopt;
}

bool is_iso2022_charset(SpecificCharacterSet charset) noexcept {
	const auto* info = charset_info_or_null(charset);
	return info && info->uses_iso_2022;
}

bool is_iso2022_g0_charset(SpecificCharacterSet charset) noexcept {
	const auto* info = charset_info_or_null(charset);
	return info && info->uses_iso_2022 &&
	    info->code_element == SpecificCharacterSetCodeElement::G0;
}

std::string_view iso2022_reset_escape() noexcept {
	const auto* info = charset_info_or_null(SpecificCharacterSet::ISO_2022_IR_6);
	return info ? info->escape_sequence_bytes : std::string_view{};
}

namespace {

std::optional<std::string> encode_base_codepoint_for_charset(
    std::uint32_t codepoint, SpecificCharacterSet target_charset, std::string* out_error) {
	const auto base_charset = base_character_set_for_charset(target_charset);
	switch (base_charset) {
	case SpecificCharacterSet::ISO_IR_100:
		if (codepoint > 0xFFu) {
			set_error(out_error, "reason=input contains characters outside ISO_IR 100");
			return std::nullopt;
		}
		return std::string(1, static_cast<char>(codepoint));
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
		const auto* table = sbcs_g1_table_for_charset(base_charset);
		if (!table) {
			set_error(out_error, "reason=missing single-byte charset table");
			return std::nullopt;
		}
		for (std::size_t i = 0; i < table->size(); ++i) {
			if ((*table)[i] == codepoint) {
				return std::string(1, static_cast<char>(0x80u + i));
			}
		}
		set_error(out_error,
		    "reason=input contains characters outside the target single-byte repertoire");
		return std::nullopt;
	}
	case SpecificCharacterSet::ISO_IR_13:
		if (codepoint >= 0xFF61u && codepoint <= 0xFF9Fu) {
			return std::string(1, static_cast<char>(0xA1u + (codepoint - 0xFF61u)));
		}
		set_error(out_error, "reason=input contains characters outside ISO_IR 13");
		return std::nullopt;
	case SpecificCharacterSet::ISO_2022_IR_149: {
		const auto multibyte = unicode_to_ksx1001_multibyte(codepoint);
		if (multibyte == 0) {
			set_error(out_error, "reason=input contains characters outside ISO 2022 IR 149");
			return std::nullopt;
		}
		std::string bytes;
		bytes.push_back(static_cast<char>(multibyte >> 8u));
		bytes.push_back(static_cast<char>(multibyte & 0xFFu));
		return bytes;
	}
	case SpecificCharacterSet::ISO_2022_IR_58: {
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
		std::string bytes;
		bytes.push_back(static_cast<char>(lead));
		bytes.push_back(static_cast<char>(trail));
		return bytes;
	}
	case SpecificCharacterSet::ISO_2022_IR_87: {
		const auto multibyte = unicode_to_jisx0208_multibyte(codepoint);
		if (multibyte == 0) {
			set_error(out_error, "reason=input contains characters outside ISO 2022 IR 87");
			return std::nullopt;
		}
		std::string bytes;
		bytes.push_back(static_cast<char>(multibyte >> 8u));
		bytes.push_back(static_cast<char>(multibyte & 0xFFu));
		return bytes;
	}
	case SpecificCharacterSet::ISO_2022_IR_159: {
		const auto multibyte = unicode_to_jisx0212_multibyte(codepoint);
		if (multibyte == 0) {
			set_error(out_error, "reason=input contains characters outside ISO 2022 IR 159");
			return std::nullopt;
		}
		std::string bytes;
		bytes.push_back(static_cast<char>(multibyte >> 8u));
		bytes.push_back(static_cast<char>(multibyte & 0xFFu));
		return bytes;
	}
	default:
		set_error(out_error, "reason=write target charset is not implemented");
		return std::nullopt;
	}
}

}  // namespace

std::optional<EncodedTextValue> encode_utf8_value_for_iso2022_charset(
    std::string_view value, SpecificCharacterSet target_charset, VR vr,
    bool reset_to_initial_each_value, std::string* out_error) {
	if (!validate_utf8(value)) {
		set_error(out_error, "reason=input is not valid UTF-8");
		return std::nullopt;
	}
	const auto* info = charset_info_or_null(target_charset);
	if (!info || !info->uses_iso_2022) {
		set_error(out_error, "reason=target charset is not ISO 2022");
		return std::nullopt;
	}

	EncodedTextValue encoded{};
	encoded.bytes.reserve(value.size() * 4u);
	const auto designate_escape = info->escape_sequence_bytes;
	const auto reset_escape = iso2022_reset_escape();
	const bool g0_designation = info->code_element == SpecificCharacterSetCodeElement::G0;
	encoded.ended_designated = reset_to_initial_each_value;
	bool use_initial_designation = reset_to_initial_each_value;

	std::size_t offset = 0;
	while (offset < value.size()) {
		std::uint32_t codepoint = 0;
		if (!decode_utf8_codepoint(value, offset, codepoint)) {
			set_error(out_error, "reason=input is not valid UTF-8");
			return std::nullopt;
		}

		if (codepoint < 0x80u) {
			if (g0_designation && encoded.ended_designated) {
				encoded.bytes.append(reset_escape.data(), reset_escape.size());
				encoded.ended_designated = false;
			}
			encoded.bytes.push_back(static_cast<char>(codepoint));
			if (is_iso2022_state_reset_char(vr, codepoint)) {
				encoded.ended_designated = !g0_designation && reset_to_initial_each_value;
				use_initial_designation = reset_to_initial_each_value;
			} else if (g0_designation) {
				use_initial_designation = false;
			}
			continue;
		}

		auto base_bytes = encode_base_codepoint_for_charset(codepoint, target_charset, out_error);
		if (!base_bytes) {
			return std::nullopt;
		}
		if (!encoded.ended_designated) {
			if (!(g0_designation && use_initial_designation)) {
				encoded.bytes.append(designate_escape.data(), designate_escape.size());
			}
			encoded.ended_designated = true;
		}
		use_initial_designation = false;
		encoded.bytes.append(base_bytes->data(), base_bytes->size());
	}
	return encoded;
}

std::optional<EncodedTextValue> encode_utf8_value_for_iso2022_charset_plan(
    std::string_view value, const ParsedSpecificCharacterSet& parsed, VR vr,
    bool start_with_initial_g1_designation, std::string* out_error) {
	if (!validate_utf8(value)) {
		set_error(out_error, "reason=input is not valid UTF-8");
		return std::nullopt;
	}
	if (!parsed.is_multi_term()) {
		return encode_utf8_value_for_iso2022_charset(
		    value, parsed.primary, vr, start_with_initial_g1_designation, out_error);
	}
	if (!charset_plan_uses_only_iso2022_terms(parsed)) {
		set_error(out_error,
		    "reason=multi-term Specific Character Set with non-ISO 2022 terms is not implemented");
		return std::nullopt;
	}

	EncodedTextValue encoded{};
	encoded.bytes.reserve(value.size() * 4u);
	const auto reset_escape = iso2022_reset_escape();
	std::optional<SpecificCharacterSet> active_g0{};
	const auto initial_g1 = first_iso2022_g1_term(parsed);
	std::optional<SpecificCharacterSet> active_g1 =
	    start_with_initial_g1_designation ? initial_g1 : std::optional<SpecificCharacterSet>{};

	std::size_t offset = 0;
	while (offset < value.size()) {
		std::uint32_t codepoint = 0;
		if (!decode_utf8_codepoint(value, offset, codepoint)) {
			set_error(out_error, "reason=input is not valid UTF-8");
			return std::nullopt;
		}

		if (codepoint < 0x80u) {
			if (active_g0) {
				encoded.bytes.append(reset_escape.data(), reset_escape.size());
				active_g0.reset();
			}
			encoded.bytes.push_back(static_cast<char>(codepoint));
			if (is_iso2022_state_reset_char(vr, codepoint)) {
				active_g0.reset();
				active_g1 = initial_g1;
			}
			encoded.ended_designated = active_g0.has_value();
			continue;
		}

		bool encoded_codepoint = false;
		for (const auto term : parsed.terms) {
			if (term == SpecificCharacterSet::NONE || term == SpecificCharacterSet::ISO_IR_6 ||
			    term == SpecificCharacterSet::ISO_2022_IR_6) {
				continue;
			}
			std::string ignored_error;
			auto base_bytes = encode_base_codepoint_for_charset(codepoint, term, &ignored_error);
			if (!base_bytes) {
				continue;
			}
			const auto* info = charset_info_or_null(term);
			if (!info || !info->uses_iso_2022) {
				continue;
			}
			if (info->code_element == SpecificCharacterSetCodeElement::G0) {
				if (!active_g0 || *active_g0 != term) {
					encoded.bytes.append(
					    info->escape_sequence_bytes.data(), info->escape_sequence_bytes.size());
					active_g0 = term;
				}
			} else if (info->code_element == SpecificCharacterSetCodeElement::G1) {
				if (!active_g1 || *active_g1 != term) {
					encoded.bytes.append(
					    info->escape_sequence_bytes.data(), info->escape_sequence_bytes.size());
					active_g1 = term;
				}
			}
			encoded.bytes.append(base_bytes->data(), base_bytes->size());
			encoded_codepoint = true;
			break;
		}

		if (!encoded_codepoint) {
			set_error(out_error,
			    "reason=input contains characters outside the declared multi-term Specific Character Set");
			return std::nullopt;
		}
		encoded.ended_designated = active_g0.has_value();
	}

	encoded.ended_designated = active_g0.has_value();
	return encoded;
}

}  // namespace dicom::charset::detail
