#include "dicom.h"

#include "pixel/encode/core/encode_context_resolver.hpp"
#include "pixel/encode/core/encode_set_pixel_data_resolver.hpp"
#include "pixel/encode/core/encode_set_pixel_data_runner.hpp"

#include <utility>

namespace dicom {

void pixel::EncoderContext::set_configured_state(uid::WellKnown transfer_syntax,
    std::string plugin_key, std::vector<std::string> option_keys,
    std::vector<pixel::CodecOptionKv> codec_options) {
	transfer_syntax_uid_ = transfer_syntax;
	plugin_key_ = std::move(plugin_key);
	option_keys_ = std::move(option_keys);
	codec_options_ = std::move(codec_options);
	configured_ = true;
}

void pixel::EncoderContext::configure(uid::WellKnown transfer_syntax) {
	auto resolved =
	    pixel::detail::resolve_encoder_context_with_default_options_or_throw(
	        transfer_syntax);
	set_configured_state(resolved.transfer_syntax, std::move(resolved.plugin_key),
	    std::move(resolved.option_keys), std::move(resolved.codec_options));
}

void pixel::EncoderContext::configure(uid::WellKnown transfer_syntax,
    std::span<const pixel::CodecOptionTextKv> codec_opt) {
	auto resolved =
	    pixel::detail::resolve_encoder_context_with_text_options_or_throw(
	        transfer_syntax, codec_opt);
	set_configured_state(resolved.transfer_syntax, std::move(resolved.plugin_key),
	    std::move(resolved.option_keys), std::move(resolved.codec_options));
}

pixel::EncoderContext pixel::create_encoder_context(
    uid::WellKnown transfer_syntax) {
	pixel::EncoderContext context{};
	context.configure(transfer_syntax);
	return context;
}

pixel::EncoderContext pixel::create_encoder_context(uid::WellKnown transfer_syntax,
    std::span<const pixel::CodecOptionTextKv> codec_opt) {
	pixel::EncoderContext context{};
	context.configure(transfer_syntax, codec_opt);
	return context;
}

void DicomFile::finalize_set_pixel_data_transfer_syntax(
    uid::WellKnown transfer_syntax) {
	set_transfer_syntax_state_only(transfer_syntax);
	pixel::detail::update_transfer_syntax_uid_element_after_set_pixel_data_or_throw(
	    *this, transfer_syntax);
}

void DicomFile::set_pixel_data(uid::WellKnown transfer_syntax,
    const pixel::PixelSource& source) {
	const auto resolved =
	    pixel::detail::resolve_set_pixel_data_request_with_default_options_or_throw(
	        path(), transfer_syntax);
	pixel::detail::run_set_pixel_data_with_resolved_codec_options(
	    *this, transfer_syntax, source, *resolved.binding, resolved.codec_options);
	finalize_set_pixel_data_transfer_syntax(transfer_syntax);
}

void DicomFile::set_pixel_data(uid::WellKnown transfer_syntax,
    const pixel::PixelSource& source, const pixel::EncoderContext& encoder_ctx) {
	pixel::detail::validate_encoder_context_for_set_pixel_data_or_throw(path(),
	    transfer_syntax, encoder_ctx.configured_, encoder_ctx.transfer_syntax_uid_);

	const auto& binding =
	    pixel::detail::resolve_set_pixel_data_binding_for_encoder_context_or_throw(
	        path(), transfer_syntax, encoder_ctx.plugin_key_);
	pixel::detail::run_set_pixel_data_with_resolved_codec_options(
	    *this, transfer_syntax, source, binding, encoder_ctx.codec_options_);
	finalize_set_pixel_data_transfer_syntax(transfer_syntax);
}

void DicomFile::set_pixel_data(uid::WellKnown transfer_syntax,
    const pixel::PixelSource& source,
    std::span<const pixel::CodecOptionTextKv> codec_opt) {
	const auto resolved =
	    pixel::detail::resolve_set_pixel_data_request_with_text_options_or_throw(
	        path(), transfer_syntax, codec_opt);
	pixel::detail::run_set_pixel_data_with_resolved_codec_options(
	    *this, transfer_syntax, source, *resolved.binding, resolved.codec_options);
	finalize_set_pixel_data_transfer_syntax(transfer_syntax);
}

} // namespace dicom
