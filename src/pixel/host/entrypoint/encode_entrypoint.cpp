#include "dicom.h"

#include "pixel/host/encode/encode_context_validation.hpp"
#include "pixel/host/encode/encode_options_policy.hpp"
#include "pixel/host/encode/encode_set_pixel_data_runner.hpp"

#include <utility>
#include <vector>

namespace dicom {

void pixel::EncoderContext::set_configured_state(uid::WellKnown transfer_syntax,
    std::vector<std::string> option_keys,
    std::vector<pixel::CodecOptionKv> codec_options) {
	transfer_syntax_uid_ = transfer_syntax;
	option_keys_ = std::move(option_keys);
	codec_options_ = std::move(codec_options);
	configured_ = true;
}

void pixel::EncoderContext::configure(uid::WellKnown transfer_syntax) {
	pixel::detail::validate_transfer_syntax_for_encode_or_throw(
	    "pixel::EncoderContext::configure", transfer_syntax);
	auto codec_options = pixel::detail::default_codec_options_for_transfer_syntax_or_throw(
	    "pixel::EncoderContext::configure", transfer_syntax);
	set_configured_state(transfer_syntax, {}, std::move(codec_options));
}

void pixel::EncoderContext::configure(uid::WellKnown transfer_syntax,
    std::span<const pixel::CodecOptionTextKv> codec_opt) {
	pixel::detail::validate_transfer_syntax_for_encode_or_throw(
	    "pixel::EncoderContext::configure", transfer_syntax);
	std::vector<std::string> option_keys{};
	auto codec_options = pixel::detail::build_codec_option_pairs_from_text_or_throw(
	    "pixel::EncoderContext::configure", "<context>", transfer_syntax, codec_opt,
	    &option_keys);
	set_configured_state(
	    transfer_syntax, std::move(option_keys), std::move(codec_options));
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

void pixel::set_pixel_data(
    DicomFile& file, const pixel::PixelSource& source,
    const pixel::EncoderContext& encoder_ctx) {
	pixel::detail::validate_encoder_context_for_set_pixel_data_or_throw(file.path(),
	    encoder_ctx.transfer_syntax_uid_, encoder_ctx.configured_,
	    encoder_ctx.transfer_syntax_uid_);
	pixel::detail::run_set_pixel_data_with_computed_codec_options(
	    file, encoder_ctx.transfer_syntax_uid_, source, encoder_ctx.codec_options_);
	file.finalize_set_pixel_data_transfer_syntax(encoder_ctx.transfer_syntax_uid_);
}

} // namespace dicom
