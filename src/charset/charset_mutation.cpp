#include "charset/charset_detail.hpp"
#include "charset/charset_mutation.hpp"
#include "charset/charset_mutation_detail.hpp"
#include "charset/text_validation.hpp"

#include <cassert>
#include <fmt/format.h>
#include <utility>

namespace dicom::charset::detail {

template <typename ValueT, typename ViewFn>
std::optional<std::vector<std::uint8_t>> encode_utf8_value_range(VR vr,
    std::span<const ValueT> values, SpecificCharacterSet target_charset, ViewFn&& to_view,
    std::string* out_error) {
	if (values.empty()) {
		return std::vector<std::uint8_t>{};
	}
	if (!vr.allows_multiple_text_values() && values.size() != 1) {
		set_error(out_error, "reason=VR requires a single text value");
		return std::nullopt;
	}

	std::string joined;
	std::size_t total_length = values.size() > 1 ? values.size() - 1 : 0;
	for (const auto& value : values) {
		total_length += to_view(value).size();
	}
	joined.reserve(total_length);
	bool previous_value_ended_designated = false;
	const auto reset_escape = iso2022_reset_escape();
	const auto* target_info = charset_info_or_null(target_charset);
	const bool reset_to_initial_each_value = target_info && target_info->uses_iso_2022;

	for (std::size_t i = 0; i < values.size(); ++i) {
		std::optional<std::string> encoded_bytes;
		bool ended_designated = false;
		if (is_iso2022_charset(target_charset)) {
			auto encoded = encode_utf8_value_for_iso2022_charset(
			    to_view(values[i]), target_charset, vr, reset_to_initial_each_value, out_error);
			if (!encoded) {
				return std::nullopt;
			}
			encoded_bytes = std::move(encoded->bytes);
			ended_designated = encoded->ended_designated;
		} else {
			encoded_bytes =
			    encode_utf8_value_for_charset(to_view(values[i]), target_charset, vr, out_error);
			if (!encoded_bytes) {
				return std::nullopt;
			}
		}
		if (i != 0) {
			if (previous_value_ended_designated && is_iso2022_g0_charset(target_charset)) {
				joined.append(reset_escape.data(), reset_escape.size());
			}
			joined.push_back('\\');
		}
		joined.append(encoded_bytes->data(), encoded_bytes->size());
		previous_value_ended_designated = ended_designated;
	}
	return std::vector<std::uint8_t>(joined.begin(), joined.end());
}

template <typename ValueT, typename ViewFn>
std::optional<std::vector<std::uint8_t>> encode_utf8_value_range(VR vr,
    std::span<const ValueT> values, const ParsedSpecificCharacterSet& target_charset,
    ViewFn&& to_view, std::string* out_error) {
	if (!target_charset.is_multi_term()) {
		return encode_utf8_value_range(
		    vr, values, target_charset.primary, std::forward<ViewFn>(to_view), out_error);
	}
	if (values.empty()) {
		return std::vector<std::uint8_t>{};
	}
	if (!vr.allows_multiple_text_values() && values.size() != 1) {
		set_error(out_error, "reason=VR requires a single text value");
		return std::nullopt;
	}

	std::string joined;
	std::size_t total_length = values.size() > 1 ? values.size() - 1 : 0;
	for (const auto& value : values) {
		total_length += to_view(value).size();
	}
	joined.reserve(total_length * 2u);

	const auto reset_escape = iso2022_reset_escape();
	const bool value_start_restores_initial_g1 = first_iso2022_g1_term(target_charset).has_value();
	bool previous_value_ended_designated = false;
	for (std::size_t i = 0; i < values.size(); ++i) {
		auto encoded = encode_utf8_value_for_iso2022_charset_plan(
		    to_view(values[i]), target_charset, vr, value_start_restores_initial_g1, out_error);
		if (!encoded) {
			return std::nullopt;
		}
		if (i != 0) {
			if (previous_value_ended_designated) {
				joined.append(reset_escape.data(), reset_escape.size());
			}
			joined.push_back('\\');
		}
		joined.append(encoded->bytes.data(), encoded->bytes.size());
		previous_value_ended_designated = encoded->ended_designated;
	}
	return std::vector<std::uint8_t>(joined.begin(), joined.end());
}

std::optional<std::vector<std::uint8_t>> encode_charset_tag(
    SpecificCharacterSet charset, std::string* out_error) {
	switch (charset) {
	case SpecificCharacterSet::NONE:
	case SpecificCharacterSet::ISO_IR_6:
		return std::vector<std::uint8_t>{};
	default: {
		const auto* info = specific_character_set_info(charset);
		if (!info) {
			set_error(out_error, "CHARSET_UNSUPPORTED reason=missing registry metadata");
			return std::nullopt;
		}
		return std::vector<std::uint8_t>(info->defined_term.begin(), info->defined_term.end());
	}
	}
}

std::optional<std::vector<std::uint8_t>> encode_charset_tag(
    std::span<const SpecificCharacterSet> charsets, std::string* out_error) {
	if (charsets.empty()) {
		return std::vector<std::uint8_t>{};
	}
	if (charsets.size() == 1) {
		return encode_charset_tag(charsets.front(), out_error);
	}

	std::string joined;
	for (std::size_t index = 0; index < charsets.size(); ++index) {
		const auto charset = charsets[index];
		if (!joined.empty()) {
			joined.push_back('\\');
		}
		if (charset == SpecificCharacterSet::NONE) {
			if (index == 0u) {
				continue;
			}
			set_error(out_error,
			    "CHARSET_UNSUPPORTED reason=multi-term Specific Character Set may contain NONE only as the first term");
			return std::nullopt;
		}
		const auto* info = specific_character_set_info(charset);
		if (!info) {
			set_error(out_error, "CHARSET_UNSUPPORTED reason=missing registry metadata");
			return std::nullopt;
		}
		joined.append(info->defined_term.begin(), info->defined_term.end());
	}
	return std::vector<std::uint8_t>(joined.begin(), joined.end());
}

std::optional<std::vector<std::uint8_t>> encode_utf8_stored_values(
    VR vr, std::span<const std::string> values, SpecificCharacterSet target_charset,
    std::string* out_error) {
	return encode_utf8_value_range(vr, values, target_charset,
	    [](const std::string& value) {
		    return std::string_view(value.data(), value.size());
	    },
	    out_error);
}

std::optional<std::vector<std::uint8_t>> encode_utf8_stored_values(VR vr,
    std::span<const std::string> values, const ParsedSpecificCharacterSet& target_charset,
    std::string* out_error) {
	return encode_utf8_value_range(vr, values, target_charset,
	    [](const std::string& value) {
		    return std::string_view(value.data(), value.size());
	    },
	    out_error);
}

bool validate_declared_charset(
    const ParsedSpecificCharacterSet& parsed, std::string* out_error) {
	if (!parsed.is_multi_term()) {
		return true;
	}
	for (std::size_t index = 0; index < parsed.terms.size(); ++index) {
		const auto term = parsed.terms[index];
		if (term == SpecificCharacterSet::NONE) {
			if (index == 0u) {
				continue;
			}
			set_error(out_error,
			    "CHARSET_UNSUPPORTED reason=multi-term Specific Character Set may contain NONE only as the first term");
			return false;
		}
	}
	if (!charset_plan_uses_only_iso2022_terms(parsed)) {
		set_error(out_error,
		    "CHARSET_UNSUPPORTED reason=multi-term Specific Character Set with non-ISO 2022 terms is not implemented");
		return false;
	}
	return true;
}

bool omit_charset_tag(const ParsedSpecificCharacterSet& parsed) noexcept {
	return parsed.terms.size() == 1 &&
	    (parsed.primary == SpecificCharacterSet::NONE ||
	        parsed.primary == SpecificCharacterSet::ISO_IR_6 ||
	        parsed.primary == SpecificCharacterSet::ISO_2022_IR_6);
}

DecodeReplacementMode decode_mode(CharsetDecodeErrorPolicy errors) noexcept {
	switch (errors) {
	case CharsetDecodeErrorPolicy::replace_fffd:
		return DecodeReplacementMode::replace_fffd;
	case CharsetDecodeErrorPolicy::replace_hex_escape:
		return DecodeReplacementMode::replace_hex_escape;
	case CharsetDecodeErrorPolicy::strict:
	default:
		return DecodeReplacementMode::strict;
	}
}

DecodeReplacementMode decode_mode_for_encode(CharsetEncodeErrorPolicy errors) noexcept {
	switch (errors) {
	case CharsetEncodeErrorPolicy::replace_qmark:
		return DecodeReplacementMode::replace_qmark;
	case CharsetEncodeErrorPolicy::replace_unicode_escape:
		return DecodeReplacementMode::replace_hex_escape;
	case CharsetEncodeErrorPolicy::strict:
	default:
		return DecodeReplacementMode::strict;
	}
}

bool sbcs_can_encode(
    std::uint32_t codepoint, const std::array<std::uint16_t, 128>& g1_table) noexcept {
	if (codepoint < 0x80u) {
		return true;
	}
	for (const auto mapped : g1_table) {
		if (mapped == codepoint) {
			return true;
		}
	}
	return false;
}

bool can_encode(std::uint32_t codepoint, SpecificCharacterSet charset) noexcept {
	if (codepoint < 0x80u) {
		return true;
	}

	switch (base_character_set_for_charset(charset)) {
	case SpecificCharacterSet::NONE:
	case SpecificCharacterSet::ISO_IR_6:
		return false;
	case SpecificCharacterSet::ISO_IR_192:
		return codepoint <= 0x10FFFFu && !(codepoint >= 0xD800u && codepoint <= 0xDFFFu);
	case SpecificCharacterSet::ISO_IR_100:
		return codepoint <= 0xFFu;
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
		const auto* table = sbcs_g1_table_for_charset(charset);
		return table && sbcs_can_encode(codepoint, *table);
	}
	case SpecificCharacterSet::ISO_IR_13:
		return codepoint >= 0xFF61u && codepoint <= 0xFF9Fu;
	case SpecificCharacterSet::GBK:
		return unicode_to_gbk_multibyte(codepoint) != 0;
	case SpecificCharacterSet::GB18030:
		return codepoint <= 0x10FFFFu && !(codepoint >= 0xD800u && codepoint <= 0xDFFFu);
	case SpecificCharacterSet::ISO_2022_IR_149:
		return unicode_to_ksx1001_multibyte(codepoint) != 0;
	case SpecificCharacterSet::ISO_2022_IR_58: {
		const auto multibyte = unicode_to_gbk_multibyte(codepoint);
		if (multibyte == 0) {
			return false;
		}
		const auto lead = static_cast<std::uint8_t>(multibyte >> 8u);
		const auto trail = static_cast<std::uint8_t>(multibyte & 0xFFu);
		return lead >= 0xA1u && lead <= 0xFEu && trail >= 0xA1u && trail <= 0xFEu;
	}
	case SpecificCharacterSet::ISO_2022_IR_87:
		return unicode_to_jisx0208_multibyte(codepoint) != 0;
	case SpecificCharacterSet::ISO_2022_IR_159:
		return unicode_to_jisx0212_multibyte(codepoint) != 0;
	default:
		return false;
	}
}

bool can_encode(std::uint32_t codepoint, const ParsedSpecificCharacterSet& target_charset) noexcept {
	if (!target_charset.is_multi_term()) {
		return can_encode(codepoint, target_charset.primary);
	}
	for (const auto term : target_charset.terms) {
		if (can_encode(codepoint, term)) {
			return true;
		}
	}
	return false;
}

std::optional<std::string> sanitize_utf8_for_charset(std::string_view value,
    const ParsedSpecificCharacterSet& target_charset, CharsetEncodeErrorPolicy errors,
    std::string* out_error, bool* out_replaced) {
	if (!validate_utf8(value)) {
		set_error(out_error, "reason=input is not valid UTF-8");
		return std::nullopt;
	}
	if (errors == CharsetEncodeErrorPolicy::strict) {
		return std::string(value);
	}

	std::string sanitized;
	sanitized.reserve(value.size() * 4u);
	std::size_t offset = 0;
	while (offset < value.size()) {
		const auto codepoint_start = offset;
		std::uint32_t codepoint = 0;
		if (!decode_utf8_codepoint(value, offset, codepoint)) {
			set_error(out_error, "reason=input is not valid UTF-8");
			return std::nullopt;
		}

		if (can_encode(codepoint, target_charset)) {
			sanitized.append(value.substr(codepoint_start, offset - codepoint_start));
			continue;
		}

		if (errors == CharsetEncodeErrorPolicy::replace_qmark) {
			sanitized.push_back('?');
			if (out_replaced) {
				*out_replaced = true;
			}
			continue;
		}
		append_unicode_escape_replacement(sanitized, codepoint);
		if (out_replaced) {
			*out_replaced = true;
		}
	}
	return sanitized;
}

}  // namespace dicom::charset::detail

namespace dicom::charset {

bool encode_utf8_for_element(DataElement& element,
    std::span<const std::string_view> values, CharsetEncodeErrorPolicy errors,
    std::string* out_error, bool* out_replaced) {
	if (out_replaced) {
		*out_replaced = false;
	}
	if (values.empty()) {
		return element.from_string_views(values);
	}
	if (!element.vr().is_string()) {
		detail::set_error(out_error, "reason=unsupported VR for from_utf8_views");
		return false;
	}
	assert(element.vr().uses_specific_character_set());
	if (!element.vr().allows_multiple_text_values() && values.size() != 1) {
		detail::set_error(out_error, "reason=VR requires a single value for from_utf8_views");
		return false;
	}

	const DataSet* dataset = nullptr;
	if (auto* parent = element.parent()) {
		dataset = parent;
	}
	auto target_charset =
	    dataset ? detail::parse_dataset_charset(*dataset, out_error)
	            : std::optional<detail::ParsedSpecificCharacterSet>{
	                  detail::default_specific_character_set_plan()};
	if (!target_charset || !detail::validate_declared_charset(*target_charset, out_error)) {
		return false;
	}

	if (errors == CharsetEncodeErrorPolicy::strict) {
		auto encoded = detail::encode_utf8_value_range(element.vr(), values, *target_charset,
		    [](std::string_view value) { return value; }, out_error);
		if (!encoded) {
			return false;
		}
		element.set_value_bytes_nocheck(std::move(*encoded));
		return true;
	}

	std::vector<std::string> owned_values;
	owned_values.reserve(values.size());
	for (const auto value : values) {
		owned_values.emplace_back(value);
	}
	for (auto& value : owned_values) {
		auto sanitized =
		    detail::sanitize_utf8_for_charset(value, *target_charset, errors, out_error, out_replaced);
		if (!sanitized) {
			return false;
		}
		value = std::move(*sanitized);
	}
	auto encoded = detail::encode_utf8_stored_values(
	    element.vr(), owned_values, *target_charset, out_error);
	if (!encoded) {
		return false;
	}
	element.set_value_bytes_nocheck(std::move(*encoded));
	return true;
}

}  // namespace dicom::charset
