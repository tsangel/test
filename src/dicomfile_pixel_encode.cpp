#include "dicom.h"

#include "pixel/host/encode/encode_context_validation.hpp"
#include "pixel/host/encode/encode_metadata_updater.hpp"

namespace dicom {

// Keep transfer syntax commit separate from the encode runner so the file state
// changes only after pixel bytes and pixel metadata were produced successfully.
void DicomFile::finalize_set_pixel_data_transfer_syntax(
    uid::WellKnown transfer_syntax) {
	set_transfer_syntax_state_only(transfer_syntax);
	pixel::detail::update_transfer_syntax_uid_element_after_set_pixel_data_or_throw(
	    *this, transfer_syntax);
}

void DicomFile::set_pixel_data(uid::WellKnown transfer_syntax,
    pixel::ConstPixelSpan source) {
	// The span overload is the new public entry point and reuses the default encoder context.
	pixel::set_pixel_data(*this, source, pixel::create_encoder_context(transfer_syntax));
}

void DicomFile::set_pixel_data(uid::WellKnown transfer_syntax,
    pixel::ConstPixelSpan source, const pixel::EncoderContext& encoder_ctx) {
	// Validate the explicit context against the requested transfer syntax before bridging.
	pixel::detail::validate_encoder_context_for_set_pixel_data_or_throw(path(),
	    transfer_syntax, encoder_ctx.configured(), encoder_ctx.transfer_syntax_uid());
	pixel::set_pixel_data(*this, source, encoder_ctx);
}

void DicomFile::set_pixel_data(uid::WellKnown transfer_syntax,
    pixel::ConstPixelSpan source,
    std::span<const pixel::CodecOptionTextKv> codec_opt) {
	pixel::set_pixel_data(
	    *this, source, pixel::create_encoder_context(transfer_syntax, codec_opt));
}

} // namespace dicom
