#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dicom.h"

namespace dicom::charset::detail {

enum class DecodeReplacementMode : std::uint8_t {
	strict = 0,
	replace_fffd,
	replace_hex_escape,
	replace_qmark,
};

inline constexpr Tag kSpecificCharacterSetTag{0x0008u, 0x0005u};

struct ParsedSpecificCharacterSet {
	std::vector<SpecificCharacterSet> terms{};
	SpecificCharacterSet primary{SpecificCharacterSet::NONE};

	[[nodiscard]] bool is_multi_term() const noexcept {
		return terms.size() > 1;
	}
};

using CharsetSpec = ParsedSpecificCharacterSet;

struct EncodedTextValue {
	std::string bytes{};
	bool ended_designated{false};
};

[[nodiscard]] bool is_supported_passthrough_charset(SpecificCharacterSet charset) noexcept;
void set_error(std::string* out_error, std::string message);
[[nodiscard]] const SpecificCharacterSetInfo* charset_info_or_null(
    SpecificCharacterSet charset) noexcept;
[[nodiscard]] SpecificCharacterSet base_character_set_for_charset(
    SpecificCharacterSet charset) noexcept;
[[nodiscard]] bool is_supported_single_byte_charset(SpecificCharacterSet charset) noexcept;
[[nodiscard]] bool is_supported_iso_2022_single_byte_charset(
    SpecificCharacterSet charset) noexcept;

[[nodiscard]] ParsedSpecificCharacterSet default_specific_character_set_plan();
[[nodiscard]] const CharsetSpec* default_charset_spec() noexcept;
[[nodiscard]] const CharsetSpec* intern_charset_spec(CharsetSpec spec);
[[nodiscard]] std::optional<ParsedSpecificCharacterSet> parse_charset_terms(
    std::span<const SpecificCharacterSet> terms, std::string_view origin, std::string* out_error);
[[nodiscard]] std::optional<ParsedSpecificCharacterSet> parse_charset_values(
    std::span<const std::string_view> values, std::string_view origin, std::string* out_error);
[[nodiscard]] std::optional<ParsedSpecificCharacterSet> parse_charset_element(
    const DataElement& element, std::string* out_error);
[[nodiscard]] const CharsetSpec* parse_dataset_charset(
    const DataSet& dataset, std::string* out_error);
[[nodiscard]] bool same_charset_terms(
    const ParsedSpecificCharacterSet& lhs, const ParsedSpecificCharacterSet& rhs) noexcept;
[[nodiscard]] bool charset_plan_uses_only_iso2022_terms(
    const ParsedSpecificCharacterSet& parsed) noexcept;
[[nodiscard]] std::optional<SpecificCharacterSet> first_iso2022_g1_term(
    const ParsedSpecificCharacterSet& parsed) noexcept;
[[nodiscard]] std::optional<SpecificCharacterSet> initial_iso2022_g1_term(
    const ParsedSpecificCharacterSet& parsed) noexcept;
[[nodiscard]] bool are_equivalent_passthrough_charsets(
    SpecificCharacterSet lhs, SpecificCharacterSet rhs) noexcept;
[[nodiscard]] bool is_iso2022_charset(SpecificCharacterSet charset) noexcept;
[[nodiscard]] bool is_iso2022_g0_charset(SpecificCharacterSet charset) noexcept;
[[nodiscard]] std::string_view iso2022_reset_escape() noexcept;
[[nodiscard]] std::string_view iso2022_initial_reset_escape(
    SpecificCharacterSet charset) noexcept;
[[nodiscard]] inline bool is_iso2022_state_reset_char(VR vr, std::uint32_t codepoint) noexcept {
	switch (codepoint) {
	case '\n':
	case '\f':
	case '\r':
	case '\t':
		return true;
	case '\\':
		return vr.allows_multiple_text_values();
	case '^':
	case '=':
		return vr == VR::PN;
	default:
		return false;
	}
}

[[nodiscard]] bool append_utf8_codepoint(std::string& out, std::uint32_t codepoint) noexcept;
[[nodiscard]] bool decode_utf8_codepoint(
    std::string_view value, std::size_t& offset, std::uint32_t& out_codepoint) noexcept;
void append_unicode_escape_replacement(std::string& out, std::uint32_t codepoint);
void append_hex_escape_replacement(std::string& out, std::span<const std::uint8_t> bytes);
void append_decode_replacement(
    std::string& out, DecodeReplacementMode mode, std::span<const std::uint8_t> bytes,
    bool* out_replaced = nullptr);

[[nodiscard]] std::uint16_t unicode_to_ksx1001_multibyte(std::uint32_t codepoint) noexcept;
[[nodiscard]] std::uint16_t unicode_to_jisx0208_multibyte(std::uint32_t codepoint) noexcept;
[[nodiscard]] std::uint16_t unicode_to_jisx0212_multibyte(std::uint32_t codepoint) noexcept;

[[nodiscard]] const std::array<std::uint16_t, 128>* sbcs_g1_table_for_charset(
    SpecificCharacterSet charset) noexcept;
[[nodiscard]] std::optional<std::string> latin1_to_utf8_string(
    std::string_view value, std::string* out_error);
[[nodiscard]] std::optional<std::string> utf8_to_latin1_string(
    std::string_view value, std::string* out_error);
[[nodiscard]] std::optional<std::string> utf8_to_sbcs_string(std::string_view value,
    const std::array<std::uint16_t, 128>& g1_table, std::string* out_error);
[[nodiscard]] std::optional<std::string> decode_sbcs_raw_to_utf8(
    std::string_view value, SpecificCharacterSet source_charset,
    DecodeReplacementMode mode = DecodeReplacementMode::strict,
    std::string* out_error = nullptr, bool* out_replaced = nullptr);

[[nodiscard]] std::uint16_t unicode_to_gbk_multibyte(std::uint32_t codepoint) noexcept;
[[nodiscard]] std::optional<std::string> gb2312_to_utf8_string(
    std::string_view value, std::string_view charset_name, std::string* out_error);
[[nodiscard]] std::optional<std::string> utf8_to_gb2312_string(
    std::string_view value, std::string* out_error);
[[nodiscard]] std::optional<std::string> gb18030_to_utf8_string(
    std::string_view value, bool allow_four_byte, std::string_view charset_name,
    DecodeReplacementMode mode = DecodeReplacementMode::strict,
    std::string* out_error = nullptr, bool* out_replaced = nullptr,
    bool stop_at_first_value = false);
[[nodiscard]] std::optional<std::string> utf8_to_gbk_string(
    std::string_view value, std::string* out_error);
[[nodiscard]] std::optional<std::string> utf8_to_gb18030_string(
    std::string_view value, std::string* out_error);

[[nodiscard]] std::optional<EncodedTextValue> encode_utf8_value_for_iso2022_charset(
    std::string_view value, SpecificCharacterSet target_charset, VR vr,
    bool reset_to_initial_each_value, std::string* out_error);
[[nodiscard]] std::optional<EncodedTextValue> encode_utf8_value_for_iso2022_charset_plan(
    std::string_view value, const ParsedSpecificCharacterSet& target_charset, VR vr,
    bool reset_to_initial_each_value, std::string* out_error);
[[nodiscard]] std::optional<std::string> encode_utf8_value_for_charset(
    std::string_view value, SpecificCharacterSet target_charset, VR vr, std::string* out_error);
[[nodiscard]] std::optional<std::string> decode_iso_2022_charset_plan_to_utf8(
    std::string_view value, const ParsedSpecificCharacterSet& declared_charset, VR vr,
    DecodeReplacementMode mode = DecodeReplacementMode::strict,
    std::string* out_error = nullptr, bool* out_replaced = nullptr,
    bool stop_at_first_value = false);

[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_utf8_stored_values(
    VR vr, std::span<const std::string> values, SpecificCharacterSet target_charset,
    std::string* out_error);
[[nodiscard]] std::optional<std::vector<std::uint8_t>> encode_utf8_stored_values(
    VR vr, std::span<const std::string> values, const ParsedSpecificCharacterSet& target_charset,
    std::string* out_error);

}  // namespace dicom::charset::detail
