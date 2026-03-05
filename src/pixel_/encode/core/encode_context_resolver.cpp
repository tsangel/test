#include "pixel_/encode/core/encode_context_resolver.hpp"

#include "diagnostics.h"
#include "pixel_/encode/core/encode_binding_resolver.hpp"

namespace dicom::pixel::detail {

namespace {

void validate_transfer_syntax_for_encode_or_throw(
    std::string_view function_name, uid::WellKnown transfer_syntax) {
	if (!transfer_syntax.valid() ||
	    transfer_syntax.uid_type() != UidType::TransferSyntax) {
		diag::error_and_throw(
		    "{} reason=transfer_syntax must be a valid Transfer Syntax UID",
		    function_name);
	}
}

} // namespace

ResolvedEncoderContext resolve_encoder_context_with_default_options_or_throw(
    uid::WellKnown transfer_syntax) {
	validate_transfer_syntax_for_encode_or_throw(
	    "pixel::EncoderContext::configure", transfer_syntax);
	const auto selected = select_encode_plugin_binding_or_throw(
	    "pixel::EncoderContext::configure", "<context>", transfer_syntax);
	if (!selected.plugin->default_options) {
		diag::error_and_throw(
		    "pixel::EncoderContext::configure ts={} plugin={} reason=codec plugin does not provide default options",
		    transfer_syntax.value(), selected.plugin->key);
	}

	ResolvedEncoderContext resolved{};
	resolved.transfer_syntax = transfer_syntax;
	resolved.plugin_key = std::string(selected.plugin->key);
	if (const auto default_error =
	        selected.plugin->default_options(transfer_syntax,
	            resolved.codec_options)) {
		diag::error_and_throw(
		    "pixel::EncoderContext::configure ts={} plugin={} reason={}",
		    transfer_syntax.value(), selected.plugin->key, *default_error);
	}
	return resolved;
}

ResolvedEncoderContext resolve_encoder_context_with_text_options_or_throw(
    uid::WellKnown transfer_syntax,
    std::span<const pixel::CodecOptionTextKv> codec_opt) {
	validate_transfer_syntax_for_encode_or_throw(
	    "pixel::EncoderContext::configure", transfer_syntax);
	const auto selected = select_encode_plugin_binding_or_throw(
	    "pixel::EncoderContext::configure", "<context>", transfer_syntax);

	ResolvedEncoderContext resolved{};
	resolved.transfer_syntax = transfer_syntax;
	resolved.plugin_key = std::string(selected.plugin->key);
	resolved.codec_options = build_codec_option_pairs_from_text_or_throw(
	    "pixel::EncoderContext::configure", "<context>", transfer_syntax,
	    selected.plugin->key, codec_opt, &resolved.option_keys);
	return resolved;
}

} // namespace dicom::pixel::detail
