#include "charset/charset_detail.hpp"
#include "charset/generated/jisx0208_tables.hpp"
#include "charset/generated/jisx0212_tables.hpp"
#include "charset/generated/ksx1001_tables.hpp"

#include <algorithm>
#include <fmt/format.h>

namespace dicom::charset::detail {

namespace {

std::optional<std::string> ksx1001_to_utf8_string(
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
			set_error(out_error, fmt::format("reason=truncated {} byte sequence", charset_name));
			return std::nullopt;
		}
		const auto b2 = static_cast<std::uint8_t>(value[offset + 1u]);
		if (b1 < 0xA1u || b1 > 0xFEu || b2 < 0xA1u || b2 > 0xFEu) {
			set_error(out_error,
			    fmt::format("reason=source byte sequence is not valid {}", charset_name));
			return std::nullopt;
		}
		const auto row_offset = tables::map_ksx1001_multibyte_to_unicode[b1 - 0xA1u];
		const auto codepoint =
		    tables::map_ksx1001_multibyte_to_unicode[row_offset + (b2 - 0xA1u)];
		if (codepoint == 0u) {
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

std::optional<std::string> jis_multibyte_to_utf8_string(std::string_view value,
    const std::uint16_t* lookup_table, std::string_view charset_name, std::string* out_error) {
	std::string utf8;
	utf8.reserve(value.size() * 3u);
	std::size_t offset = 0;
	while (offset < value.size()) {
		const auto b1 = static_cast<std::uint8_t>(value[offset]);
		if (b1 < 0x80u) {
			if (b1 == '\t' || b1 == '\n' || b1 == '\f' || b1 == '\r' || b1 == '^' || b1 == '=') {
				utf8.push_back(static_cast<char>(b1));
				++offset;
				continue;
			}
			if (offset + 1u >= value.size()) {
				set_error(out_error, fmt::format("reason=truncated {} byte sequence", charset_name));
				return std::nullopt;
			}
			const auto b2 = static_cast<std::uint8_t>(value[offset + 1u]);
			if (b1 < 0x21u || b1 > 0x7Eu || b2 < 0x21u || b2 > 0x7Eu) {
				set_error(out_error,
				    fmt::format("reason=source byte sequence is not valid {}", charset_name));
				return std::nullopt;
			}
			const auto row_offset = lookup_table[b1 - 0x21u];
			const auto codepoint = lookup_table[row_offset + (b2 - 0x21u)];
			if (codepoint == 0u) {
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
		set_error(out_error, fmt::format("reason=source byte sequence is not valid {}", charset_name));
		return std::nullopt;
	}
	return utf8;
}

std::optional<std::string> decode_iso_2022_single_byte_to_utf8(
    std::string_view value, SpecificCharacterSet declared_charset, std::string* out_error) {
	const auto* declared_info = charset_info_or_null(declared_charset);
	if (!declared_info || !declared_info->uses_iso_2022) {
		set_error(out_error, "reason=declared charset is not ISO 2022");
		return std::nullopt;
	}

	const auto base_charset = base_character_set_for_charset(declared_charset);
	const auto* reset_info = charset_info_or_null(SpecificCharacterSet::ISO_2022_IR_6);
	const auto reset_escape =
	    reset_info ? reset_info->escape_sequence_bytes : std::string_view{};
	const auto designated_escape = declared_info->escape_sequence_bytes;

	std::string stripped;
	stripped.reserve(value.size());
	std::size_t offset = 0;
	while (offset < value.size()) {
		const auto ch = static_cast<std::uint8_t>(value[offset]);
		if (ch == 0x1Bu) {
			const auto remaining = value.substr(offset);
			if (!designated_escape.empty() && remaining.starts_with(designated_escape)) {
				offset += designated_escape.size();
				continue;
			}
			if (!reset_escape.empty() && remaining.starts_with(reset_escape)) {
				offset += reset_escape.size();
				continue;
			}
			set_error(out_error, "reason=unsupported ISO 2022 escape sequence");
			return std::nullopt;
		}
		stripped.push_back(value[offset]);
		++offset;
	}
	return decode_sbcs_raw_to_utf8(
	    stripped, base_charset, DecodeReplacementMode::strict, out_error);
}

std::optional<std::string> decode_iso_2022_multibyte_to_utf8(
    std::string_view value, SpecificCharacterSet declared_charset, VR vr, std::string* out_error) {
	const auto* declared_info = charset_info_or_null(declared_charset);
	if (!declared_info || !declared_info->uses_iso_2022) {
		set_error(out_error, "reason=declared charset is not ISO 2022");
		return std::nullopt;
	}

	const auto* reset_info = charset_info_or_null(SpecificCharacterSet::ISO_2022_IR_6);
	const auto reset_escape =
	    reset_info ? reset_info->escape_sequence_bytes : std::string_view{};
	const auto designated_escape = declared_info->escape_sequence_bytes;

	if (declared_charset == SpecificCharacterSet::ISO_2022_IR_149) {
		std::string stripped;
		stripped.reserve(value.size());
		std::size_t offset = 0;
		while (offset < value.size()) {
			const auto ch = static_cast<std::uint8_t>(value[offset]);
			if (ch != 0x1Bu) {
				stripped.push_back(value[offset]);
				++offset;
				continue;
			}
			const auto remaining = value.substr(offset);
			if (!designated_escape.empty() && remaining.starts_with(designated_escape)) {
				offset += designated_escape.size();
				continue;
			}
			if (!reset_escape.empty() && remaining.starts_with(reset_escape)) {
				offset += reset_escape.size();
				continue;
			}
			set_error(out_error, "reason=unsupported ISO 2022 escape sequence");
			return std::nullopt;
		}
		return ksx1001_to_utf8_string(stripped, "ISO 2022 IR 149", out_error);
	}
	if (declared_charset == SpecificCharacterSet::ISO_2022_IR_58) {
		std::string stripped;
		stripped.reserve(value.size());
		std::size_t offset = 0;
		while (offset < value.size()) {
			const auto ch = static_cast<std::uint8_t>(value[offset]);
			if (ch != 0x1Bu) {
				stripped.push_back(value[offset]);
				++offset;
				continue;
			}
			const auto remaining = value.substr(offset);
			if (!designated_escape.empty() && remaining.starts_with(designated_escape)) {
				offset += designated_escape.size();
				continue;
			}
			if (!reset_escape.empty() && remaining.starts_with(reset_escape)) {
				offset += reset_escape.size();
				continue;
			}
			set_error(out_error, "reason=unsupported ISO 2022 escape sequence");
			return std::nullopt;
		}
		return gb2312_to_utf8_string(stripped, "ISO 2022 IR 58", out_error);
	}

	enum class Mode : std::uint8_t { ascii, designated };
	const Mode initial_mode =
	    value.find('\x1b') == std::string_view::npos ? Mode::designated : Mode::ascii;
	Mode mode = initial_mode;

	std::string utf8;
	utf8.reserve(value.size() * 3u);
	std::size_t offset = 0;
	while (offset < value.size()) {
		const auto ch = static_cast<std::uint8_t>(value[offset]);
		if (ch == 0x1Bu) {
			const auto remaining = value.substr(offset);
			if (!designated_escape.empty() && remaining.starts_with(designated_escape)) {
				mode = Mode::designated;
				offset += designated_escape.size();
				continue;
			}
			if (!reset_escape.empty() && remaining.starts_with(reset_escape)) {
				mode = Mode::ascii;
				offset += reset_escape.size();
				continue;
			}
			set_error(out_error, "reason=unsupported ISO 2022 escape sequence");
			return std::nullopt;
		}

		if (mode == Mode::ascii) {
			if (!append_utf8_codepoint(utf8, ch)) {
				set_error(out_error, "reason=failed to encode source byte as UTF-8");
				return std::nullopt;
			}
			if (is_iso2022_state_reset_char(vr, ch)) {
				mode = initial_mode;
			}
			++offset;
			continue;
		}

		if (ch < 0x80u && is_iso2022_state_reset_char(vr, ch)) {
			if (!append_utf8_codepoint(utf8, ch)) {
				set_error(out_error, "reason=failed to encode source byte as UTF-8");
				return std::nullopt;
			}
			mode = initial_mode;
			++offset;
			continue;
		}

		std::optional<std::string> converted;
		switch (declared_charset) {
		case SpecificCharacterSet::ISO_2022_IR_87:
			converted = jis_multibyte_to_utf8_string(
			    value.substr(offset, std::min<std::size_t>(2u, value.size() - offset)),
			    tables::map_jisx0208_multibyte_to_unicode, "ISO 2022 IR 87", out_error);
			break;
		case SpecificCharacterSet::ISO_2022_IR_159:
			converted = jis_multibyte_to_utf8_string(
			    value.substr(offset, std::min<std::size_t>(2u, value.size() - offset)),
			    tables::map_jisx0212_multibyte_to_unicode, "ISO 2022 IR 159", out_error);
			break;
		default:
			set_error(out_error, "reason=ISO 2022 multibyte charset is not implemented");
			return std::nullopt;
		}
		if (!converted) {
			return std::nullopt;
		}
		utf8.append(*converted);
		offset += 2u;
	}
	return utf8;
}

std::optional<SpecificCharacterSet> match_supported_iso2022_escape(
    std::string_view remaining, std::size_t& matched_length) noexcept {
	SpecificCharacterSet matched_charset = SpecificCharacterSet::Unknown;
	matched_length = 0;
	for (const auto& info : kSpecificCharacterSetInfo) {
		if (!info.uses_iso_2022 || info.escape_sequence_bytes.empty()) {
			continue;
		}
		if (!is_supported_iso_2022_single_byte_charset(info.value) &&
		    info.value != SpecificCharacterSet::ISO_2022_IR_149 &&
		    info.value != SpecificCharacterSet::ISO_2022_IR_58 &&
		    info.value != SpecificCharacterSet::ISO_2022_IR_87 &&
		    info.value != SpecificCharacterSet::ISO_2022_IR_159) {
			continue;
		}
		if (info.escape_sequence_bytes.size() <= matched_length) {
			continue;
		}
		if (remaining.starts_with(info.escape_sequence_bytes)) {
			matched_charset = info.value;
			matched_length = info.escape_sequence_bytes.size();
		}
	}
	if (matched_charset == SpecificCharacterSet::Unknown) {
		return std::nullopt;
	}
	return matched_charset;
}

std::optional<std::string> decode_raw_iso2022_bytes_to_utf8(
    std::string_view value, SpecificCharacterSet active_charset, std::string* out_error) {
	if (is_supported_iso_2022_single_byte_charset(active_charset)) {
		return decode_sbcs_raw_to_utf8(
		    value, base_character_set_for_charset(active_charset),
		    DecodeReplacementMode::strict, out_error);
	}
	switch (active_charset) {
	case SpecificCharacterSet::ISO_2022_IR_149:
	case SpecificCharacterSet::ISO_2022_IR_58:
	case SpecificCharacterSet::ISO_2022_IR_87:
	case SpecificCharacterSet::ISO_2022_IR_159:
		return decode_iso_2022_multibyte_to_utf8(value, active_charset, VR::LO, out_error);
	default:
		set_error(out_error, "reason=active ISO 2022 charset is not implemented");
		return std::nullopt;
	}
}

}  // namespace

std::optional<std::string> decode_iso_2022_charset_plan_to_utf8(
    std::string_view value, const ParsedSpecificCharacterSet& parsed, VR vr,
    DecodeReplacementMode mode, std::string* out_error, bool* out_replaced,
    bool stop_at_first_value) {
	if (!charset_plan_uses_only_iso2022_terms(parsed)) {
		set_error(out_error,
		    "reason=multi-term Specific Character Set with non-ISO 2022 terms is not implemented");
		return std::nullopt;
	}

	const std::optional<SpecificCharacterSet> initial_g0 =
	    [&parsed]() -> std::optional<SpecificCharacterSet> {
		    if (parsed.terms.size() != 1) {
			    return std::nullopt;
		    }
		    const auto* info = charset_info_or_null(parsed.primary);
		    if (!info || !info->uses_iso_2022 ||
		        info->code_element != SpecificCharacterSetCodeElement::G0) {
			    return std::nullopt;
		    }
		    return parsed.primary;
	    }();
	const std::optional<SpecificCharacterSet> initial_g1 = first_iso2022_g1_term(parsed);
	std::optional<SpecificCharacterSet> active_g0 = initial_g0;
	std::optional<SpecificCharacterSet> active_g1 = initial_g1;

	const auto reset_escape = iso2022_reset_escape();
	std::string utf8;
	utf8.reserve(value.size() * 3u);
	std::size_t offset = 0;
	while (offset < value.size()) {
		const auto ch = static_cast<std::uint8_t>(value[offset]);
		if (ch == 0x1Bu) {
			const auto remaining = value.substr(offset);
			if (!reset_escape.empty() && remaining.starts_with(reset_escape)) {
				active_g0.reset();
				offset += reset_escape.size();
				continue;
			}
			std::size_t matched_length = 0;
			const auto matched_charset = match_supported_iso2022_escape(remaining, matched_length);
			if (!matched_charset) {
				if (mode != DecodeReplacementMode::strict) {
					const auto consume = std::min<std::size_t>(4u, remaining.size());
					append_decode_replacement(
					    utf8, mode,
					    std::span<const std::uint8_t>(
					        reinterpret_cast<const std::uint8_t*>(remaining.data()), consume), out_replaced);
					offset += consume;
					continue;
				}
				set_error(out_error, "reason=unsupported ISO 2022 escape sequence");
				return std::nullopt;
			}
			const auto* info = charset_info_or_null(*matched_charset);
			if (!info) {
				set_error(out_error, "reason=missing registry metadata");
				return std::nullopt;
			}
			if (info->code_element == SpecificCharacterSetCodeElement::G0) {
				active_g0 = *matched_charset;
			} else if (info->code_element == SpecificCharacterSetCodeElement::G1) {
				active_g1 = *matched_charset;
			}
			offset += matched_length;
			continue;
		}

		if (active_g0) {
			if (offset + 1u >= value.size()) {
				if (mode != DecodeReplacementMode::strict) {
					append_decode_replacement(
					    utf8, mode,
					    std::span<const std::uint8_t>(
					        reinterpret_cast<const std::uint8_t*>(value.data()) + offset,
					        value.size() - offset), out_replaced);
					offset = value.size();
					continue;
				}
				set_error(out_error, "reason=truncated ISO 2022 multibyte sequence");
				return std::nullopt;
			}
			auto converted =
			    decode_raw_iso2022_bytes_to_utf8(value.substr(offset, 2u), *active_g0, out_error);
			if (!converted) {
				if (mode != DecodeReplacementMode::strict) {
					append_decode_replacement(
					    utf8, mode,
					    std::span<const std::uint8_t>(
					        reinterpret_cast<const std::uint8_t*>(value.data()) + offset, 2u), out_replaced);
					offset += 2u;
					continue;
				}
				return std::nullopt;
			}
			utf8.append(*converted);
			offset += 2u;
			continue;
		}

		if (ch < 0x80u) {
			if (stop_at_first_value && ch == '\\') {
				break;
			}
			utf8.push_back(static_cast<char>(ch));
			if (is_iso2022_state_reset_char(vr, ch)) {
				active_g0 = initial_g0;
				active_g1 = initial_g1;
			}
			++offset;
			continue;
		}

		if (!active_g1) {
			if (mode != DecodeReplacementMode::strict) {
				append_decode_replacement(
				    utf8, mode,
				    std::span<const std::uint8_t>(
				        reinterpret_cast<const std::uint8_t*>(value.data()) + offset, 1u), out_replaced);
				++offset;
				continue;
			}
			set_error(out_error,
			    "reason=non-ASCII byte encountered without an active ISO 2022 G1 charset");
			return std::nullopt;
		}

		const auto width = (*active_g1 == SpecificCharacterSet::ISO_2022_IR_149 ||
		        *active_g1 == SpecificCharacterSet::ISO_2022_IR_58)
		        ? 2u
		        : 1u;
		if (offset + width > value.size()) {
			if (mode != DecodeReplacementMode::strict) {
				append_decode_replacement(
				    utf8, mode,
				    std::span<const std::uint8_t>(
				        reinterpret_cast<const std::uint8_t*>(value.data()) + offset,
				        value.size() - offset), out_replaced);
				offset = value.size();
				continue;
			}
			set_error(out_error, "reason=truncated ISO 2022 sequence");
			return std::nullopt;
		}
		auto converted =
		    decode_raw_iso2022_bytes_to_utf8(value.substr(offset, width), *active_g1, out_error);
		if (!converted) {
			if (mode != DecodeReplacementMode::strict) {
				append_decode_replacement(
				    utf8, mode,
				    std::span<const std::uint8_t>(
				        reinterpret_cast<const std::uint8_t*>(value.data()) + offset, width), out_replaced);
				offset += width;
				continue;
			}
			return std::nullopt;
		}
		utf8.append(*converted);
		offset += width;
	}
	return utf8;
}

}  // namespace dicom::charset::detail
