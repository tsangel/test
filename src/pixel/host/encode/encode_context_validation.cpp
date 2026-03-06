#include "pixel/host/encode/encode_context_validation.hpp"

#include "diagnostics.h"

namespace dicom::pixel::detail {

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

} // namespace dicom::pixel::detail
