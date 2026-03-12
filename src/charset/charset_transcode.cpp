#include "charset/charset_decode.hpp"
#include "charset/charset_detail.hpp"
#include "charset/charset_mutation.hpp"
#include "charset/charset_mutation_detail.hpp"
#include "charset/text_validation.hpp"

#include <fmt/format.h>

namespace dicom::charset::detail {

namespace {

constexpr bool is_trim_char(char ch) noexcept {
	return ch == ' ' || ch == '\0';
}

void trim_leading(std::string_view& view) noexcept {
	while (!view.empty() && is_trim_char(view.front())) {
		view.remove_prefix(1);
	}
}

void trim_trailing(std::string_view& view) noexcept {
	while (!view.empty() && is_trim_char(view.back())) {
		view.remove_suffix(1);
	}
}

std::string_view trim_decoded_component(std::string_view value, bool trim_front) noexcept {
	if (trim_front) {
		trim_leading(value);
	}
	trim_trailing(value);
	return value;
}

std::optional<std::vector<std::string>> split_decoded_text_values(
    VR vr, const std::string& decoded) {
	std::vector<std::string> values;
	const auto push_single = [&](bool trim_front) -> std::optional<std::vector<std::string>> {
		auto view = trim_decoded_component(decoded, trim_front);
		values.emplace_back(view);
		return values;
	};

	switch (static_cast<std::uint16_t>(vr)) {
	case VR::AE_val:
	case VR::AS_val:
	case VR::CS_val:
	case VR::DA_val:
	case VR::DS_val:
	case VR::DT_val:
	case VR::IS_val:
	case VR::LO_val:
	case VR::PN_val:
	case VR::SH_val:
	case VR::TM_val:
	case VR::UI_val:
	case VR::UC_val: {
		const bool trim_front = vr != VR::UC;
		std::size_t start = 0;
		while (start <= decoded.size()) {
			const auto next = decoded.find('\\', start);
			const auto len =
			    (next == std::string::npos) ? (decoded.size() - start) : (next - start);
			auto token = std::string_view(decoded).substr(start, len);
			token = trim_decoded_component(token, trim_front);
			values.emplace_back(token);
			if (next == std::string::npos) {
				break;
			}
			start = next + 1;
		}
		return values;
	}
	case VR::UR_val:
		return push_single(true);
	case VR::LT_val:
	case VR::ST_val:
	case VR::UT_val:
		return push_single(false);
	default:
		return std::nullopt;
	}
}

std::string_view raw_text_bytes(const DataElement& element) noexcept {
	const auto span = element.value_span();
	return std::string_view(reinterpret_cast<const char*>(span.data()), span.size());
}

std::string_view raw_prefix_before_delimiter(std::string_view raw_value) noexcept {
	const auto delimiter = raw_value.find('\\');
	return delimiter == std::string_view::npos ? raw_value : raw_value.substr(0, delimiter);
}

std::optional<std::string> normalize_decoded_first_value(VR vr, const std::string& decoded) {
	switch (static_cast<std::uint16_t>(vr)) {
	case VR::AE_val:
	case VR::AS_val:
	case VR::CS_val:
	case VR::DA_val:
	case VR::DS_val:
	case VR::DT_val:
	case VR::IS_val:
	case VR::LO_val:
	case VR::PN_val:
	case VR::SH_val:
	case VR::TM_val:
	case VR::UI_val:
	{
		auto view = trim_decoded_component(decoded, true);
		return std::string(view);
	}
	case VR::UC_val:
	case VR::LT_val:
	case VR::ST_val:
	case VR::UT_val:
	{
		auto view = trim_decoded_component(decoded, false);
		return std::string(view);
	}
	case VR::UR_val:
	{
		auto view = trim_decoded_component(decoded, true);
		return std::string(view);
	}
	default:
		return std::nullopt;
	}
}

std::optional<std::string> decode_raw_text_to_utf8(
    std::string_view raw_value, const DataElement& element,
    const ParsedSpecificCharacterSet& source_charset_plan, DecodeReplacementMode decode_mode,
    std::string* out_error, bool* out_replaced, bool stop_at_first_value = false) {
	if (source_charset_plan.is_multi_term()) {
		auto converted = decode_iso_2022_charset_plan_to_utf8(
		    raw_value, source_charset_plan, element.vr(), decode_mode, out_error, out_replaced,
		    stop_at_first_value);
		if (!converted) {
			if (out_error && !out_error->empty()) {
				*out_error =
				    fmt::format("CHARSET_UNSUPPORTED tag={} {}", element.tag().to_string(), *out_error);
			}
			return std::nullopt;
		}
		return converted;
	}

	const auto source_charset = source_charset_plan.primary;
	if (is_iso2022_charset(source_charset)) {
		ParsedSpecificCharacterSet single_term_plan{{source_charset}, source_charset};
		auto converted = decode_iso_2022_charset_plan_to_utf8(
		    raw_value, single_term_plan, element.vr(), decode_mode, out_error, out_replaced,
		    stop_at_first_value);
		if (!converted) {
			if (out_error && !out_error->empty()) {
				*out_error =
				    fmt::format("CHARSET_UNSUPPORTED tag={} {}", element.tag().to_string(), *out_error);
			}
			return std::nullopt;
		}
		return converted;
	}

	switch (source_charset) {
	case SpecificCharacterSet::NONE:
	case SpecificCharacterSet::ISO_IR_6:
		if (stop_at_first_value && element.vr().allows_multiple_text_values()) {
			raw_value = raw_prefix_before_delimiter(raw_value);
		}
		if (decode_mode == DecodeReplacementMode::strict && !validate_ascii(raw_value)) {
			set_error(out_error,
			    fmt::format(
			        "CHARSET_UNSUPPORTED tag={} reason=element value is not valid for source charset",
			        element.tag().to_string()));
			return std::nullopt;
		}
		if (decode_mode == DecodeReplacementMode::strict) {
			return std::string(raw_value);
		}
		return decode_sbcs_raw_to_utf8(raw_value, source_charset, decode_mode, out_error, out_replaced);
	case SpecificCharacterSet::ISO_IR_192:
	{
		if (stop_at_first_value && element.vr().allows_multiple_text_values()) {
			raw_value = raw_prefix_before_delimiter(raw_value);
		}
		if (decode_mode == DecodeReplacementMode::strict && !validate_utf8(raw_value)) {
			set_error(out_error,
			    fmt::format(
			        "CHARSET_UNSUPPORTED tag={} reason=element value is not valid for source charset",
			        element.tag().to_string()));
			return std::nullopt;
		}
		if (decode_mode == DecodeReplacementMode::strict) {
			return std::string(raw_value);
		}
		std::string converted;
		converted.reserve(raw_value.size() * 3u);
		std::size_t offset = 0;
		while (offset < raw_value.size()) {
			const auto start = offset;
			std::uint32_t codepoint = 0;
			if (decode_utf8_codepoint(raw_value, offset, codepoint)) {
				converted.append(raw_value.substr(start, offset - start));
				continue;
			}
			const auto byte = static_cast<std::uint8_t>(raw_value[start]);
			append_decode_replacement(
			    converted, decode_mode, std::span<const std::uint8_t>(&byte, 1), out_replaced);
			offset = start + 1u;
		}
		return converted;
	}
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
	case SpecificCharacterSet::ISO_IR_166: {
		if (stop_at_first_value && element.vr().allows_multiple_text_values()) {
			raw_value = raw_prefix_before_delimiter(raw_value);
		}
		auto converted =
		    decode_sbcs_raw_to_utf8(raw_value, source_charset, decode_mode, out_error, out_replaced);
		if (!converted && out_error && !out_error->empty()) {
			*out_error =
			    fmt::format("CHARSET_UNSUPPORTED tag={} {}", element.tag().to_string(), *out_error);
		}
		return converted;
	}
	case SpecificCharacterSet::GB18030: {
		auto converted = gb18030_to_utf8_string(
		    raw_value, true, "GB18030", decode_mode, out_error, out_replaced, stop_at_first_value);
		if (!converted && out_error && !out_error->empty()) {
			*out_error =
			    fmt::format("CHARSET_UNSUPPORTED tag={} {}", element.tag().to_string(), *out_error);
		}
		return converted;
	}
	case SpecificCharacterSet::GBK: {
		auto converted = gb18030_to_utf8_string(
		    raw_value, false, "GBK", decode_mode, out_error, out_replaced, stop_at_first_value);
		if (!converted && out_error && !out_error->empty()) {
			*out_error =
			    fmt::format("CHARSET_UNSUPPORTED tag={} {}", element.tag().to_string(), *out_error);
		}
		return converted;
	}
	default:
		set_error(out_error,
		    fmt::format("CHARSET_UNSUPPORTED tag={} term={} reason=read charset is not implemented",
		        element.tag().to_string(),
		        specific_character_set_info(source_charset)
		            ? specific_character_set_info(source_charset)->defined_term
		            : std::string_view{"unknown"}));
		return std::nullopt;
	}
}

}  // namespace

std::optional<std::string> decode_text_value(
    const DataElement& element, const ParsedSpecificCharacterSet& source_charset_plan,
    DecodeReplacementMode decode_mode, std::string* out_error, bool* out_replaced) {
	auto converted = decode_raw_text_to_utf8(
	    raw_text_bytes(element), element, source_charset_plan, decode_mode, out_error,
	    out_replaced, element.vr().allows_multiple_text_values());
	if (!converted) {
		return std::nullopt;
	}
	return normalize_decoded_first_value(element.vr(), *converted);
}

std::optional<std::vector<std::string>> decode_text_values(
    const DataElement& element, const ParsedSpecificCharacterSet& source_charset_plan,
    DecodeReplacementMode decode_mode, std::string* out_error, bool* out_replaced) {
	auto converted = decode_raw_text_to_utf8(
	    raw_text_bytes(element), element, source_charset_plan, decode_mode, out_error,
	    out_replaced);
	if (!converted) {
		return std::nullopt;
	}
	return split_decoded_text_values(element.vr(), *converted);
}

std::optional<std::vector<std::string>> decode_text_values(
    const DataElement& element, DecodeReplacementMode decode_mode, std::string* out_error,
    bool* out_replaced) {
	const DataSet* dataset = nullptr;
	if (auto* parent = element.parent()) {
		dataset = parent;
	}
	auto source_charset_plan =
	    dataset ? parse_dataset_charset(*dataset, out_error)
	            : std::optional<ParsedSpecificCharacterSet>{default_specific_character_set_plan()};
	if (!source_charset_plan) {
		return std::nullopt;
	}
	return decode_text_values(element, *source_charset_plan, decode_mode, out_error, out_replaced);
}

std::optional<std::string> decode_text_value(
    const DataElement& element, DecodeReplacementMode decode_mode, std::string* out_error,
    bool* out_replaced) {
	const DataSet* dataset = nullptr;
	if (auto* parent = element.parent()) {
		dataset = parent;
	}
	auto source_charset_plan =
	    dataset ? parse_dataset_charset(*dataset, out_error)
	            : std::optional<ParsedSpecificCharacterSet>{default_specific_character_set_plan()};
	if (!source_charset_plan) {
		return std::nullopt;
	}
	return decode_text_value(element, *source_charset_plan, decode_mode, out_error, out_replaced);
}

bool should_reuse_raw_values(const ParsedSpecificCharacterSet& source_charset,
    const ParsedSpecificCharacterSet& target_charset) noexcept {
	if (same_charset_terms(source_charset, target_charset)) {
		return true;
	}
	if (!source_charset.is_multi_term() && !target_charset.is_multi_term()) {
		return are_equivalent_passthrough_charsets(
		    source_charset.primary, target_charset.primary);
	}
	return false;
}

std::optional<std::vector<std::uint8_t>> transcode_element_text(
    const DataElement& element, const ParsedSpecificCharacterSet& source_charset,
    const ParsedSpecificCharacterSet& target_charset, CharsetEncodeErrorPolicy errors,
    std::string* out_error, bool* out_replaced) {
	const auto utf8_values =
	    decode_text_values(element, source_charset, decode_mode_for_encode(errors), out_error, out_replaced);
	if (!utf8_values) {
		set_error(out_error,
		    fmt::format(
		        "CHARSET_UNSUPPORTED tag={} vr={} reason=failed to convert source text to UTF-8 before charset mutation",
		        element.tag().to_string(), element.vr().str()));
		return std::nullopt;
	}

	if (errors == CharsetEncodeErrorPolicy::strict) {
		auto encoded =
		    encode_utf8_stored_values(element.vr(), *utf8_values, target_charset, out_error);
		if (!encoded && out_error && !out_error->empty()) {
			*out_error =
			    fmt::format("tag={} vr={} {}", element.tag().to_string(), element.vr().str(), *out_error);
		}
		return encoded;
	}

	std::vector<std::string> sanitized_values;
	sanitized_values.reserve(utf8_values->size());
	for (const auto& utf8_value : *utf8_values) {
		auto sanitized =
		    sanitize_utf8_for_charset(utf8_value, target_charset, errors, out_error, out_replaced);
		if (!sanitized) {
			if (out_error && !out_error->empty()) {
				*out_error = fmt::format(
				    "tag={} vr={} {}", element.tag().to_string(), element.vr().str(), *out_error);
			}
			return std::nullopt;
		}
		sanitized_values.push_back(std::move(*sanitized));
	}
	auto encoded =
	    encode_utf8_stored_values(element.vr(), sanitized_values, target_charset, out_error);
	if (!encoded && out_error && !out_error->empty()) {
		*out_error =
		    fmt::format("tag={} vr={} {}", element.tag().to_string(), element.vr().str(), *out_error);
	}
	return encoded;
}

bool validate_charset_values(DataSet& dataset,
    const ParsedSpecificCharacterSet& target_charset, std::string* out_error,
    bool* out_replaced) {
	auto source_charset = parse_dataset_charset(dataset, out_error);
	if (!source_charset || !validate_declared_charset(*source_charset, out_error)) {
		return false;
	}
	const bool reuse_raw_values = should_reuse_raw_values(*source_charset, target_charset);

	for (auto& element : dataset) {
		if (element.tag() == kSpecificCharacterSetTag) {
			continue;
		}
		if (auto* sequence = element.as_sequence()) {
			for (const auto& item_dataset_ptr : *sequence) {
				if (item_dataset_ptr &&
				    !validate_charset_values(*item_dataset_ptr, target_charset, out_error, out_replaced)) {
					return false;
				}
			}
			continue;
		}
		if (!element.vr().uses_specific_character_set() || reuse_raw_values) {
			continue;
		}
		if (!transcode_element_text(
		        element, *source_charset, target_charset, CharsetEncodeErrorPolicy::strict,
		        out_error, out_replaced)) {
			return false;
		}
	}
	return true;
}

bool rewrite_charset_values(DataSet& dataset,
    const ParsedSpecificCharacterSet& target_charset, CharsetEncodeErrorPolicy errors,
    std::string* out_error, bool* out_replaced) {
	auto source_charset = parse_dataset_charset(dataset, out_error);
	if (!source_charset || !validate_declared_charset(*source_charset, out_error)) {
		return false;
	}
	const bool reuse_raw_values = should_reuse_raw_values(*source_charset, target_charset);

	for (auto& element : dataset) {
		if (element.tag() == kSpecificCharacterSetTag) {
			continue;
		}
		if (auto* sequence = element.as_sequence()) {
			for (const auto& item_dataset_ptr : *sequence) {
				if (item_dataset_ptr &&
				    !rewrite_charset_values(
				        *item_dataset_ptr, target_charset, errors, out_error, out_replaced)) {
					return false;
				}
			}
			continue;
		}
		if (!element.vr().uses_specific_character_set() || reuse_raw_values) {
			continue;
		}
		auto encoded = transcode_element_text(
		    element, *source_charset, target_charset, errors, out_error, out_replaced);
		if (!encoded) {
			return false;
		}
		element.set_value_bytes_nocheck(std::move(*encoded));
	}
	return apply_declared_charset(dataset, target_charset, out_error);
}

bool apply_declared_charset(
    DataSet& dataset, const ParsedSpecificCharacterSet& parsed, std::string* out_error) {
	if (omit_charset_tag(parsed)) {
		dataset.remove_dataelement_nocheck(kSpecificCharacterSetTag);
		return true;
	}

	auto encoded_tag_value = encode_charset_tag(parsed.terms, out_error);
	if (!encoded_tag_value) {
		return false;
	}
	auto& element = dataset.add_dataelement_nocheck(kSpecificCharacterSetTag, VR::CS);
	element.set_value_bytes_nocheck(std::move(*encoded_tag_value));
	return true;
}

DataSet& ensure_dataset_loaded_for_edit(DataSet& dataset) {
	dataset.ensure_loaded(Tag(0xFFFFu, 0xFFFFu));
	return dataset;
}

}  // namespace dicom::charset::detail

namespace dicom::charset {

std::optional<std::string> raw_element_as_owned_utf8_value(
    const DataElement& element, std::string* out_error) {
	return detail::decode_text_value(
	    element, detail::DecodeReplacementMode::strict, out_error, nullptr);
}

std::optional<std::string> raw_element_as_owned_utf8_value(
    const DataElement& element, CharsetDecodeErrorPolicy errors, std::string* out_error) {
	return detail::decode_text_value(element, detail::decode_mode(errors), out_error, nullptr);
}

std::optional<std::string> raw_element_as_owned_utf8_value(
    const DataElement& element, CharsetDecodeErrorPolicy errors, std::string* out_error,
    bool* out_replaced) {
	if (out_replaced) {
		*out_replaced = false;
	}
	return detail::decode_text_value(element, detail::decode_mode(errors), out_error, out_replaced);
}

std::optional<std::vector<std::string>> raw_element_as_owned_utf8_values(
    const DataElement& element, std::string* out_error) {
	return detail::decode_text_values(
	    element, detail::DecodeReplacementMode::strict, out_error, nullptr);
}

std::optional<std::vector<std::string>> raw_element_as_owned_utf8_values(
    const DataElement& element, CharsetDecodeErrorPolicy errors, std::string* out_error) {
	return detail::decode_text_values(element, detail::decode_mode(errors), out_error, nullptr);
}

std::optional<std::vector<std::string>> raw_element_as_owned_utf8_values(
    const DataElement& element, CharsetDecodeErrorPolicy errors, std::string* out_error,
    bool* out_replaced) {
	if (out_replaced) {
		*out_replaced = false;
	}
	return detail::decode_text_values(element, detail::decode_mode(errors), out_error, out_replaced);
}

bool set_dataset_declared_charset(DataSet& dataset,
    std::span<const SpecificCharacterSet> charsets, std::string* out_error) {
	auto& target_dataset = detail::ensure_dataset_loaded_for_edit(dataset);

	auto parsed = detail::parse_charset_terms(charsets, "DataSet", out_error);
	if (!parsed || !detail::validate_declared_charset(*parsed, out_error)) {
		return false;
	}
	return detail::apply_declared_charset(target_dataset, *parsed, out_error);
}

bool transcode_dataset_charset(DataSet& dataset, std::span<const SpecificCharacterSet> charsets,
    CharsetEncodeErrorPolicy errors, std::string* out_error, bool* out_replaced) {
	if (out_replaced) {
		*out_replaced = false;
	}
	auto& target_dataset = detail::ensure_dataset_loaded_for_edit(dataset);

	auto target_charset = detail::parse_charset_terms(charsets, "DataSet", out_error);
	if (!target_charset || !detail::validate_declared_charset(*target_charset, out_error)) {
		return false;
	}

	if (errors == CharsetEncodeErrorPolicy::strict) {
		if (!detail::validate_charset_values(target_dataset, *target_charset, out_error, out_replaced)) {
			return false;
		}
		if (!detail::rewrite_charset_values(
		        target_dataset, *target_charset, errors, out_error, out_replaced)) {
			return false;
		}
	} else {
		if (!detail::rewrite_charset_values(
		        target_dataset, *target_charset, errors, out_error, out_replaced)) {
			return false;
		}
	}
	return true;
}

}  // namespace dicom::charset
