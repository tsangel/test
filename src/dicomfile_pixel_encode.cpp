#include "dicom.h"

#include "pixel/host/error/codec_error.hpp"
#include "diagnostics.h"

namespace dicom {
void DicomFile::set_pixel_data(uid::WellKnown transfer_syntax,
    pixel::ConstPixelSpan source) {
	// The span overload is the new public entry point and reuses the default encoder context.
	pixel::set_pixel_data(*this, source, pixel::create_encoder_context(transfer_syntax));
}

void DicomFile::set_pixel_data(uid::WellKnown transfer_syntax,
    pixel::ConstPixelSpan source, const pixel::EncoderContext& encoder_ctx) {
	try {
		const auto ctx_ts = encoder_ctx.transfer_syntax_uid();
		// Reject an explicit encoder context whose configured transfer syntax disagrees
		// with the public API argument before forwarding into the shared entrypoint.
		if (encoder_ctx.configured() && ctx_ts.valid() && ctx_ts != transfer_syntax) {
			pixel::detail::throw_codec_stage_exception(
			    pixel::detail::CodecStatusCode::invalid_argument,
			    "validate_encoder_context",
			    "encoder context transfer syntax mismatch (ctx_ts={})",
			    ctx_ts.value());
		}
	} catch (const diag::DicomException& ex) {
		pixel::detail::rethrow_codec_exception_at_boundary_or_throw(
		    "DicomFile::set_pixel_data", *this, transfer_syntax, ex);
	}
	pixel::set_pixel_data(*this, source, encoder_ctx);
}

void DicomFile::set_pixel_data(uid::WellKnown transfer_syntax,
    pixel::ConstPixelSpan source,
    std::span<const pixel::CodecOptionTextKv> codec_opt) {
	pixel::set_pixel_data(
	    *this, source, pixel::create_encoder_context(transfer_syntax, codec_opt));
}

} // namespace dicom
