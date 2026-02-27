#include "pixel/encode/core/encode_target_resolver.hpp"

#include "diagnostics.h"

namespace dicom::pixel::detail {

PixelEncodeTarget classify_pixel_encode_target(
    const TransferSyntaxPluginBinding& binding) noexcept {
	PixelEncodeTarget target{};
	target.is_native_uncompressed =
	    binding.profile == CodecProfile::native_uncompressed;
	target.is_encapsulated_uncompressed =
	    binding.profile == CodecProfile::encapsulated_uncompressed;
	target.is_rle = binding.profile == CodecProfile::rle_lossless;
	target.is_j2k = binding.plugin_key == "jpeg2k";
	target.is_j2k_lossless =
	    binding.profile == CodecProfile::jpeg2000_lossless;
	target.is_j2k_lossy =
	    binding.profile == CodecProfile::jpeg2000_lossy;
	target.is_htj2k = binding.plugin_key == "htj2k";
	target.is_htj2k_lossless =
	    binding.profile == CodecProfile::htj2k_lossless ||
	    binding.profile == CodecProfile::htj2k_lossless_rpcl;
	target.is_htj2k_lossy = binding.profile == CodecProfile::htj2k_lossy;
	target.is_jpegls = binding.plugin_key == "jpegls";
	target.is_jpegls_lossless =
	    binding.profile == CodecProfile::jpegls_lossless;
	target.is_jpegls_lossy =
	    binding.profile == CodecProfile::jpegls_near_lossless;
	target.is_jpeg = binding.plugin_key == "jpeg";
	target.is_jpeg_lossless = binding.profile == CodecProfile::jpeg_lossless;
	target.is_jpeg_lossy = binding.profile == CodecProfile::jpeg_lossy;
	target.is_jpegxl = binding.plugin_key == "jpegxl";
	target.is_jpegxl_lossless =
	    binding.profile == CodecProfile::jpegxl_lossless;
	target.is_jpegxl_lossy = binding.profile == CodecProfile::jpegxl_lossy;
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
