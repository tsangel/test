#include "pixel/encode/core/encode_target_resolver.hpp"

#include "diagnostics.h"

namespace dicom::pixel::detail {

PixelEncodeTarget classify_pixel_encode_target(
    uid::WellKnown transfer_syntax) noexcept {
	PixelEncodeTarget target{};
	target.is_native_uncompressed =
	    transfer_syntax.is_uncompressed() && !transfer_syntax.is_encapsulated();
	target.is_encapsulated_uncompressed =
	    transfer_syntax.is_uncompressed() && transfer_syntax.is_encapsulated();
	target.is_rle = transfer_syntax.is_rle();
	target.is_j2k = transfer_syntax.is_jpeg2000();
	target.is_j2k_lossless = target.is_j2k && transfer_syntax.is_lossless();
	target.is_j2k_lossy = target.is_j2k && transfer_syntax.is_lossy();
	target.is_htj2k = transfer_syntax.is_htj2k();
	target.is_htj2k_lossless = target.is_htj2k && transfer_syntax.is_lossless();
	target.is_htj2k_lossy = target.is_htj2k && transfer_syntax.is_lossy();
	target.is_jpegls = transfer_syntax.is_jpegls();
	target.is_jpegls_lossless = target.is_jpegls && transfer_syntax.is_lossless();
	target.is_jpegls_lossy = target.is_jpegls && transfer_syntax.is_lossy();
	target.is_jpeg = transfer_syntax.is_jpeg_family() && !target.is_jpegls &&
	    !target.is_j2k && !target.is_htj2k && !transfer_syntax.is_jpegxl();
	target.is_jpeg_lossless = target.is_jpeg && transfer_syntax.is_lossless();
	target.is_jpeg_lossy = target.is_jpeg && transfer_syntax.is_lossy();
	target.is_jpegxl = transfer_syntax.is_jpegxl();
	target.is_jpegxl_lossless = target.is_jpegxl && transfer_syntax.is_lossless();
	target.is_jpegxl_lossy = target.is_jpegxl && transfer_syntax.is_lossy();
	return target;
}

void validate_target_source_constraints(const PixelEncodeTarget& target,
    int bits_allocated, int bits_stored, std::string_view file_path) {
	if ((target.is_j2k || target.is_htj2k || target.is_jpegls || target.is_jpeg ||
	        target.is_jpegxl) &&
	    bits_allocated > 16) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=selected encoder currently supports bits_allocated <= 16",
		    file_path);
	}
	if (target.is_jpeg_lossy && bits_stored > 12) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=lossy JPEG transfer syntax requires bits_stored <= 12",
		    file_path);
	}
}

pixel::Photometric resolve_output_photometric(const PixelEncodeTarget& target,
    bool use_multicomponent_transform,
    pixel::Photometric source_photometric) noexcept {
	if (!use_multicomponent_transform) {
		return source_photometric;
	}
	if (target.is_j2k_lossless || target.is_htj2k_lossless) {
		return pixel::Photometric::ybr_rct;
	}
	return pixel::Photometric::ybr_ict;
}

bool target_uses_lossy_compression(const PixelEncodeTarget& target) noexcept {
	return target.is_j2k_lossy || target.is_htj2k_lossy ||
	    target.is_jpegls_lossy || target.is_jpeg_lossy ||
	    target.is_jpegxl_lossy;
}

std::optional<std::string_view> lossy_method_for_target(
    const PixelEncodeTarget& target) noexcept {
	if (target.is_j2k_lossy) {
		return std::string_view("ISO_15444_1");
	}
	if (target.is_htj2k_lossy) {
		return std::string_view("ISO_15444_15");
	}
	if (target.is_jpegls_lossy) {
		return std::string_view("ISO_14495_1");
	}
	if (target.is_jpeg_lossy) {
		return std::string_view("ISO_10918_1");
	}
	if (target.is_jpegxl_lossy) {
		return std::string_view("ISO_18181_1");
	}
	return std::nullopt;
}

} // namespace dicom::pixel::detail
