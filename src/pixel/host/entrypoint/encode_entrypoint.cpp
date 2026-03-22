#include "dicom.h"

#include "diagnostics.h"
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
    DicomFile& file, pixel::ConstPixelSpan source,
    const pixel::EncoderContext& encoder_ctx) {
	if (!encoder_ctx.configured()) {
		dicom::diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=encoder context is not configured",
		    file.path());
	}
	if (!encoder_ctx.transfer_syntax_uid().valid()) {
		dicom::diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=encoder context transfer syntax is not valid",
		    file.path());
	}
	pixel::detail::run_set_pixel_data_with_computed_codec_options(
	    file, encoder_ctx.transfer_syntax_uid_, source, encoder_ctx.codec_options_);
	file.set_transfer_syntax_state_only(encoder_ctx.transfer_syntax_uid_);
	if (!file.set_value("(0002,0010)", VR::UI,
	        encoder_ctx.transfer_syntax_uid_.value())) {
		dicom::diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=failed to update (0002,0010) TransferSyntaxUID",
		    file.path());
	}
}

} // namespace dicom
