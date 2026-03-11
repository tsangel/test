#include "charset/charset_decode.hpp"
#include "charset/charset_detail.hpp"
#include "charset/charset_mutation.hpp"
#include "charset/charset_mutation_detail.hpp"
#include "charset/text_validation.hpp"

#include <fmt/format.h>

namespace dicom::charset::detail {

std::optional<std::vector<std::string>> decode_text_values(
    const DataElement& element, const ParsedSpecificCharacterSet& source_charset_plan,
    DecodeReplacementMode decode_mode, std::string* out_error, bool* out_replaced) {
	const auto values = element.to_string_views();
	if (!values) {
		return std::nullopt;
	}

	std::vector<std::string> utf8_values;
	utf8_values.reserve(values->size());
	for (const auto raw_value : *values) {
		if (source_charset_plan.is_multi_term()) {
			auto converted = decode_iso_2022_charset_plan_to_utf8(
			    raw_value, source_charset_plan, element.vr(), decode_mode, out_error, out_replaced);
			if (!converted) {
				if (out_error && !out_error->empty()) {
					*out_error = fmt::format(
					    "CHARSET_UNSUPPORTED tag={} {}", element.tag().to_string(), *out_error);
				}
				return std::nullopt;
			}
			utf8_values.push_back(std::move(*converted));
			continue;
		}

		const auto source_charset = source_charset_plan.primary;
		if (is_iso2022_charset(source_charset)) {
			ParsedSpecificCharacterSet single_term_plan{{source_charset}, source_charset};
			auto converted = decode_iso_2022_charset_plan_to_utf8(
			    raw_value, single_term_plan, element.vr(), decode_mode, out_error, out_replaced);
			if (!converted) {
				if (out_error && !out_error->empty()) {
					*out_error = fmt::format(
					    "CHARSET_UNSUPPORTED tag={} {}", element.tag().to_string(), *out_error);
				}
				return std::nullopt;
			}
			utf8_values.push_back(std::move(*converted));
			continue;
		}

		switch (source_charset) {
		case SpecificCharacterSet::NONE:
		case SpecificCharacterSet::ISO_IR_6:
			if (decode_mode == DecodeReplacementMode::strict && !validate_ascii(raw_value)) {
				set_error(out_error,
				    fmt::format(
				        "CHARSET_UNSUPPORTED tag={} reason=element value is not valid for source charset",
				        element.tag().to_string()));
				return std::nullopt;
			}
			if (decode_mode == DecodeReplacementMode::strict) {
				utf8_values.emplace_back(raw_value);
			} else {
				auto converted = decode_sbcs_raw_to_utf8(
				    raw_value, source_charset, decode_mode, out_error, out_replaced);
				if (!converted) {
					return std::nullopt;
				}
				utf8_values.push_back(std::move(*converted));
			}
			break;
		case SpecificCharacterSet::ISO_IR_192:
			if (decode_mode == DecodeReplacementMode::strict && !validate_utf8(raw_value)) {
				set_error(out_error,
				    fmt::format(
				        "CHARSET_UNSUPPORTED tag={} reason=element value is not valid for source charset",
				        element.tag().to_string()));
				return std::nullopt;
			}
			if (decode_mode == DecodeReplacementMode::strict) {
				utf8_values.emplace_back(raw_value);
			} else {
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
				utf8_values.push_back(std::move(converted));
			}
			break;
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
			auto converted =
			    decode_sbcs_raw_to_utf8(raw_value, source_charset, decode_mode, out_error, out_replaced);
			if (!converted) {
				if (out_error && !out_error->empty()) {
					*out_error = fmt::format(
					    "CHARSET_UNSUPPORTED tag={} {}", element.tag().to_string(), *out_error);
				}
				return std::nullopt;
			}
			utf8_values.push_back(std::move(*converted));
			break;
		}
		case SpecificCharacterSet::GB18030: {
			auto converted = gb18030_to_utf8_string(
			    raw_value, true, "GB18030", decode_mode, out_error, out_replaced);
			if (!converted) {
				if (out_error && !out_error->empty()) {
					*out_error = fmt::format(
					    "CHARSET_UNSUPPORTED tag={} {}", element.tag().to_string(), *out_error);
				}
				return std::nullopt;
			}
			utf8_values.push_back(std::move(*converted));
			break;
		}
		case SpecificCharacterSet::GBK: {
			auto converted =
			    gb18030_to_utf8_string(raw_value, false, "GBK", decode_mode, out_error, out_replaced);
			if (!converted) {
				if (out_error && !out_error->empty()) {
					*out_error = fmt::format(
					    "CHARSET_UNSUPPORTED tag={} {}", element.tag().to_string(), *out_error);
				}
				return std::nullopt;
			}
			utf8_values.push_back(std::move(*converted));
			break;
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
	return utf8_values;
}

std::optional<std::vector<std::string>> decode_text_values(
    const DataElement& element, DecodeReplacementMode decode_mode, std::string* out_error,
    bool* out_replaced) {
	const DataSet* dataset = nullptr;
	if (auto* parent = element.parent()) {
		dataset = parent->root_dataset();
		if (!dataset) {
			dataset = parent;
		}
	}
	auto source_charset_plan =
	    dataset ? parse_dataset_charset(*dataset, out_error)
	            : std::optional<ParsedSpecificCharacterSet>{default_specific_character_set_plan()};
	if (!source_charset_plan) {
		return std::nullopt;
	}
	return decode_text_values(element, *source_charset_plan, decode_mode, out_error, out_replaced);
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

bool prepare_charset_mutation(DataSet& dataset,
    const ParsedSpecificCharacterSet& source_charset,
    const ParsedSpecificCharacterSet& target_charset, bool reuse_raw_values,
    CharsetEncodeErrorPolicy errors,
    PreparedCharsetMutation& prepared, std::string* out_error, bool* out_replaced) {
	for (auto& element : dataset) {
		if (element.tag() == kSpecificCharacterSetTag) {
			continue;
		}
		if (auto* sequence = element.as_sequence()) {
			for (const auto& item_dataset_ptr : *sequence) {
				if (item_dataset_ptr &&
				    !prepare_charset_mutation(*item_dataset_ptr, source_charset, target_charset,
				        reuse_raw_values, errors, prepared, out_error, out_replaced)) {
					return false;
				}
			}
			continue;
		}
		if (!element.vr().uses_specific_character_set() || reuse_raw_values) {
			continue;
		}
		auto encoded = transcode_element_text(
		    element, source_charset, target_charset, errors, out_error, out_replaced);
		if (!encoded) {
			return false;
		}
		prepared.encoded_values.emplace(&element, std::move(*encoded));
	}
	return true;
}

bool rewrite_charset_values(DataSet& dataset,
    const ParsedSpecificCharacterSet& source_charset,
    const ParsedSpecificCharacterSet& target_charset, bool reuse_raw_values,
    CharsetEncodeErrorPolicy errors,
    std::string* out_error, bool* out_replaced) {
	for (auto& element : dataset) {
		if (element.tag() == kSpecificCharacterSetTag) {
			continue;
		}
		if (auto* sequence = element.as_sequence()) {
			for (const auto& item_dataset_ptr : *sequence) {
				if (item_dataset_ptr &&
				    !rewrite_charset_values(*item_dataset_ptr, source_charset, target_charset,
				        reuse_raw_values, errors, out_error, out_replaced)) {
					return false;
				}
			}
			continue;
		}
		if (!element.vr().uses_specific_character_set() || reuse_raw_values) {
			continue;
		}
		auto encoded = transcode_element_text(
		    element, source_charset, target_charset, errors, out_error, out_replaced);
		if (!encoded) {
			return false;
		}
		element.set_value_bytes(std::move(*encoded));
	}
	return true;
}

bool apply_declared_charset(
    DataSet& dataset, const ParsedSpecificCharacterSet& parsed, std::string* out_error) {
	if (omit_charset_tag(parsed)) {
		dataset.remove_dataelement(kSpecificCharacterSetTag);
		return true;
	}

	auto encoded_tag_value = encode_charset_tag(parsed.terms, out_error);
	if (!encoded_tag_value) {
		return false;
	}
	auto& element = dataset.add_dataelement(kSpecificCharacterSetTag, VR::CS);
	element.set_value_bytes(std::move(*encoded_tag_value));
	return true;
}

DataSet& root_dataset_for_edit(DataSet& dataset) {
	if (auto* root = dataset.root_dataset()) {
		root->ensure_loaded(Tag(0xFFFFu, 0xFFFFu));
		return *root;
	}
	dataset.ensure_loaded(Tag(0xFFFFu, 0xFFFFu));
	return dataset;
}

}  // namespace dicom::charset::detail

namespace dicom::charset {

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
	auto& root_dataset = detail::root_dataset_for_edit(dataset);

	auto parsed = detail::parse_charset_terms(charsets, "DataSet", out_error);
	if (!parsed || !detail::validate_declared_charset(*parsed, out_error)) {
		return false;
	}
	return detail::apply_declared_charset(root_dataset, *parsed, out_error);
}

bool transcode_dataset_charset(DataSet& dataset, std::span<const SpecificCharacterSet> charsets,
    CharsetEncodeErrorPolicy errors, std::string* out_error, bool* out_replaced) {
	if (out_replaced) {
		*out_replaced = false;
	}
	auto& root_dataset = detail::root_dataset_for_edit(dataset);

	auto target_charset = detail::parse_charset_terms(charsets, "DataSet", out_error);
	if (!target_charset || !detail::validate_declared_charset(*target_charset, out_error)) {
		return false;
	}

	auto source_charset = detail::parse_dataset_charset(root_dataset, out_error);
	if (!source_charset || !detail::validate_declared_charset(*source_charset, out_error)) {
		return false;
	}

	const bool reuse_raw_values = detail::should_reuse_raw_values(*source_charset, *target_charset);
	if (reuse_raw_values) {
		return detail::apply_declared_charset(root_dataset, *target_charset, out_error);
	}

	if (errors == CharsetEncodeErrorPolicy::strict) {
		detail::PreparedCharsetMutation prepared{};
		if (!detail::prepare_charset_mutation(root_dataset, *source_charset, *target_charset,
		        reuse_raw_values, errors, prepared, out_error, out_replaced)) {
			return false;
		}
		for (auto& [element, encoded] : prepared.encoded_values) {
			element->set_value_bytes(std::move(encoded));
		}
	} else {
		if (!detail::rewrite_charset_values(root_dataset, *source_charset, *target_charset,
		        reuse_raw_values, errors, out_error, out_replaced)) {
			return false;
		}
	}
	return detail::apply_declared_charset(root_dataset, *target_charset, out_error);
}

}  // namespace dicom::charset
