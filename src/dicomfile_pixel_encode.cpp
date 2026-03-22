#include "dicom.h"

#include "diagnostics.h"

namespace dicom {

void DicomFile::set_pixel_data(uid::WellKnown transfer_syntax,
    pixel::ConstPixelSpan source) {
	// The span overload is the new public entry point and reuses the default encoder context.
	pixel::set_pixel_data(*this, source, pixel::create_encoder_context(transfer_syntax));
}

void DicomFile::set_pixel_data(uid::WellKnown transfer_syntax,
    pixel::ConstPixelSpan source, const pixel::EncoderContext& encoder_ctx) {
	if (!encoder_ctx.configured()) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} ts={} reason=encoder context is not configured",
		    path(), transfer_syntax.value());
	}
	if (!encoder_ctx.transfer_syntax_uid().valid() ||
	    encoder_ctx.transfer_syntax_uid() != transfer_syntax) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} ts={} ctx_ts={} reason=encoder context transfer syntax mismatch",
		    path(), transfer_syntax.value(), encoder_ctx.transfer_syntax_uid().value());
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
