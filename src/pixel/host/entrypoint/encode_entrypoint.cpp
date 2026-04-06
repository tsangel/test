#include "dicom.h"

#include "pixel/host/encode/encode_options_policy.hpp"
#include "pixel/host/encode/encode_set_pixel_data_runner.hpp"
#include "pixel/host/error/codec_error.hpp"
#include "diagnostics.h"
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
	pixel::detail::validate_transfer_syntax_for_encode_or_throw(transfer_syntax);
	auto codec_options =
	    pixel::detail::default_codec_options_for_transfer_syntax_or_throw(
	        transfer_syntax);
	set_configured_state(transfer_syntax, {}, std::move(codec_options));
}

void pixel::EncoderContext::configure(uid::WellKnown transfer_syntax,
    std::span<const pixel::CodecOptionTextKv> codec_opt) {
	pixel::detail::validate_transfer_syntax_for_encode_or_throw(transfer_syntax);
	std::vector<std::string> option_keys{};
	auto codec_options = pixel::detail::build_codec_option_pairs_from_text_or_throw(
	    transfer_syntax, codec_opt, &option_keys);
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
	try {
		// Validate the shared encoder entrypoint contract before mutating the destination dataset.
		if (!encoder_ctx.configured_) {
			pixel::detail::throw_codec_stage_exception(
			    pixel::detail::CodecStatusCode::invalid_argument,
			    "validate_encoder_context",
			    "encoder context is not configured");
		}
		if (!encoder_ctx.transfer_syntax_uid_.valid()) {
			pixel::detail::throw_codec_stage_exception(
			    pixel::detail::CodecStatusCode::invalid_argument,
			    "validate_encoder_context",
			    "encoder context transfer syntax is invalid");
		}
		pixel::detail::run_set_pixel_data_with_computed_codec_options(
		    file, encoder_ctx.transfer_syntax_uid_, source, encoder_ctx.codec_options_);
		// Commit transfer syntax state only after PixelData bytes and related pixel
		// metadata were produced successfully.
		file.set_transfer_syntax_state_only(encoder_ctx.transfer_syntax_uid_);
		if (!file.set_value(Tag(0x0002u, 0x0010u), VR::UI,
		        encoder_ctx.transfer_syntax_uid_.value())) {
			pixel::detail::throw_codec_stage_exception(
			    pixel::detail::CodecStatusCode::internal_error,
			    "finalize_transfer_syntax",
			    "failed to update (0002,0010) TransferSyntaxUID");
		}
	} catch (const diag::DicomException& ex) {
		pixel::detail::rethrow_codec_exception_at_boundary_or_throw(
		    "DicomFile::set_pixel_data", file, encoder_ctx.transfer_syntax_uid_, ex);
	}
}

void pixel::set_pixel_data(DicomFile& file, pixel::ConstPixelSpan source,
    std::size_t frame_index, const pixel::EncoderContext& encoder_ctx) {
	try {
		if (!encoder_ctx.configured()) {
			pixel::detail::throw_codec_stage_exception(
			    pixel::detail::CodecStatusCode::invalid_argument,
			    "validate_encoder_context",
			    "encoder context is not configured");
		}
		if (!encoder_ctx.transfer_syntax_uid().valid()) {
			pixel::detail::throw_codec_stage_exception(
			    pixel::detail::CodecStatusCode::invalid_argument,
			    "validate_encoder_context",
			    "encoder context transfer syntax is invalid");
		}
		pixel::detail::run_set_pixel_data_frame_with_computed_codec_options(
		    file, encoder_ctx.transfer_syntax_uid(), source, frame_index,
		    encoder_ctx.codec_options());
		file.set_transfer_syntax_state_only(encoder_ctx.transfer_syntax_uid());
		if (!file.set_value(Tag(0x0002u, 0x0010u), VR::UI,
		        encoder_ctx.transfer_syntax_uid().value())) {
			pixel::detail::throw_codec_stage_exception(
			    pixel::detail::CodecStatusCode::internal_error,
			    "finalize_transfer_syntax",
			    "failed to update (0002,0010) TransferSyntaxUID");
		}
	} catch (const diag::DicomException& ex) {
		pixel::detail::rethrow_codec_exception_at_boundary_or_throw(
		    "DicomFile::set_pixel_data", file,
		    encoder_ctx.transfer_syntax_uid(), ex);
	}
}

} // namespace dicom
