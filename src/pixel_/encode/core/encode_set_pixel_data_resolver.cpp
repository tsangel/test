#include "pixel_/encode/core/encode_set_pixel_data_resolver.hpp"

#include "diagnostics.h"

namespace dicom {

namespace pixel::detail {
using namespace dicom::literals;

namespace {

void validate_transfer_syntax_for_set_pixel_data_or_throw(
    uid::WellKnown transfer_syntax) {
	if (!transfer_syntax.valid() ||
	    transfer_syntax.uid_type() != UidType::TransferSyntax) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data reason=transfer_syntax must be a valid Transfer Syntax UID");
	}
}

codec_option_pairs build_default_codec_options_for_set_pixel_data_or_throw(
    std::string_view file_path, uid::WellKnown transfer_syntax,
    std::string_view plugin_key, codec_default_options_fn default_options) {
	if (!default_options) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} ts={} plugin={} reason=codec plugin does not provide default options",
		    file_path, transfer_syntax.value(), plugin_key);
	}
	codec_option_pairs codec_options{};
	if (const auto default_error =
	        default_options(transfer_syntax, codec_options)) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} ts={} plugin={} reason={}",
		    file_path, transfer_syntax.value(), plugin_key, *default_error);
	}
	return codec_options;
}

} // namespace

ResolvedSetPixelDataRequest
resolve_set_pixel_data_request_with_default_options_or_throw(
    std::string_view file_path, uid::WellKnown transfer_syntax) {
	validate_transfer_syntax_for_set_pixel_data_or_throw(transfer_syntax);
	const auto selected = select_encode_plugin_binding_or_throw(
	    "DicomFile::set_pixel_data", file_path, transfer_syntax);

	ResolvedSetPixelDataRequest resolved{};
	resolved.binding = selected.binding;
	resolved.codec_options = build_default_codec_options_for_set_pixel_data_or_throw(
	    file_path, transfer_syntax, selected.plugin->key,
	    selected.plugin->default_options);
	return resolved;
}

ResolvedSetPixelDataRequest
resolve_set_pixel_data_request_with_text_options_or_throw(
    std::string_view file_path, uid::WellKnown transfer_syntax,
    std::span<const pixel::CodecOptionTextKv> codec_opt) {
	validate_transfer_syntax_for_set_pixel_data_or_throw(transfer_syntax);
	const auto selected = select_encode_plugin_binding_or_throw(
	    "DicomFile::set_pixel_data", file_path, transfer_syntax);

	ResolvedSetPixelDataRequest resolved{};
	resolved.binding = selected.binding;
	resolved.codec_options = build_codec_option_pairs_from_text_or_throw(
	    "DicomFile::set_pixel_data", file_path, transfer_syntax,
	    selected.plugin->key, codec_opt);
	return resolved;
}

void validate_encoder_context_for_set_pixel_data_or_throw(
    std::string_view file_path, uid::WellKnown transfer_syntax,
    bool encoder_context_configured, uid::WellKnown encoder_context_transfer_syntax) {
	if (!encoder_context_configured) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} ts={} reason=encoder context is not configured",
		    file_path, transfer_syntax.value());
	}
	if (!encoder_context_transfer_syntax.valid() ||
	    encoder_context_transfer_syntax != transfer_syntax) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} ts={} ctx_ts={} reason=encoder context transfer syntax mismatch",
		    file_path, transfer_syntax.value(),
		    encoder_context_transfer_syntax.value());
	}
}

const TransferSyntaxPluginBinding&
resolve_set_pixel_data_binding_for_encoder_context_or_throw(
    std::string_view file_path, uid::WellKnown transfer_syntax,
    std::string_view expected_plugin_key) {
	validate_transfer_syntax_for_set_pixel_data_or_throw(transfer_syntax);
	const auto selected = select_encode_plugin_binding_or_throw(
	    "DicomFile::set_pixel_data", file_path, transfer_syntax);
	if (selected.plugin->key != expected_plugin_key) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} ts={} plugin={} ctx_plugin={} reason=encoder context plugin mismatch",
		    file_path, transfer_syntax.value(), selected.plugin->key,
		    expected_plugin_key);
	}
	return *selected.binding;
}

void update_transfer_syntax_uid_element_after_set_pixel_data_or_throw(
    DicomFile& file, uid::WellKnown transfer_syntax) {
	DataElement* transfer_syntax_element =
	    file.add_dataelement("(0002,0010)"_tag, VR::UI);
	if (!transfer_syntax_element ||
	    !transfer_syntax_element->from_transfer_syntax_uid(transfer_syntax)) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=failed to update (0002,0010) TransferSyntaxUID",
		    file.path());
	}
}

} // namespace pixel::detail

} // namespace dicom
