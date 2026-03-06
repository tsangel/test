#include "pixel/host/encode/encode_target_policy.hpp"

#include "diagnostics.h"

namespace dicom::pixel::detail {

namespace {

[[nodiscard]] bool is_jpeg_family_encode_profile(
    uint32_t codec_profile_code) noexcept {
	return codec_profile_code == PIXEL_CODEC_PROFILE_JPEG_LOSSLESS_V2 ||
	    codec_profile_code == PIXEL_CODEC_PROFILE_JPEG_LOSSY_V2;
}

[[nodiscard]] bool is_jpegls_encode_profile(uint32_t codec_profile_code) noexcept {
	return codec_profile_code == PIXEL_CODEC_PROFILE_JPEGLS_LOSSLESS_V2 ||
	    codec_profile_code == PIXEL_CODEC_PROFILE_JPEGLS_NEAR_LOSSLESS_V2;
}

[[nodiscard]] bool is_jpegxl_encode_profile(uint32_t codec_profile_code) noexcept {
	return codec_profile_code == PIXEL_CODEC_PROFILE_JPEGXL_LOSSLESS_V2 ||
	    codec_profile_code == PIXEL_CODEC_PROFILE_JPEGXL_LOSSY_V2 ||
	    codec_profile_code == PIXEL_CODEC_PROFILE_JPEGXL_JPEG_RECOMPRESSION_V2;
}

[[nodiscard]] bool is_16bit_limited_encode_profile(
    uint32_t codec_profile_code) noexcept {
	// This covers JPEG 2000, HTJ2K, JPEG-LS, JPEG family, and JPEG XL encode profiles.
	return codec_profile_code == PIXEL_CODEC_PROFILE_JPEG_LOSSLESS_V2 ||
	    codec_profile_code == PIXEL_CODEC_PROFILE_JPEG_LOSSY_V2 ||
	    codec_profile_code == PIXEL_CODEC_PROFILE_JPEGLS_LOSSLESS_V2 ||
	    codec_profile_code == PIXEL_CODEC_PROFILE_JPEGLS_NEAR_LOSSLESS_V2 ||
	    codec_profile_code == PIXEL_CODEC_PROFILE_JPEG2000_LOSSLESS_V2 ||
	    codec_profile_code == PIXEL_CODEC_PROFILE_JPEG2000_LOSSY_V2 ||
	    codec_profile_code == PIXEL_CODEC_PROFILE_HTJ2K_LOSSLESS_V2 ||
	    codec_profile_code == PIXEL_CODEC_PROFILE_HTJ2K_LOSSLESS_RPCL_V2 ||
	    codec_profile_code == PIXEL_CODEC_PROFILE_HTJ2K_LOSSY_V2 ||
	    codec_profile_code == PIXEL_CODEC_PROFILE_JPEGXL_LOSSLESS_V2 ||
	    codec_profile_code == PIXEL_CODEC_PROFILE_JPEGXL_LOSSY_V2 ||
	    codec_profile_code == PIXEL_CODEC_PROFILE_JPEGXL_JPEG_RECOMPRESSION_V2;
}

} // namespace

bool is_native_uncompressed_encode_profile(uint32_t codec_profile_code) noexcept {
	return codec_profile_code == PIXEL_CODEC_PROFILE_NATIVE_UNCOMPRESSED_V2;
}

bool is_rle_encode_profile(uint32_t codec_profile_code) noexcept {
	return codec_profile_code == PIXEL_CODEC_PROFILE_RLE_LOSSLESS_V2;
}

bool is_jpeg2000_encode_profile(uint32_t codec_profile_code) noexcept {
	return codec_profile_code == PIXEL_CODEC_PROFILE_JPEG2000_LOSSLESS_V2 ||
	    codec_profile_code == PIXEL_CODEC_PROFILE_JPEG2000_LOSSY_V2;
}

bool is_htj2k_encode_profile(uint32_t codec_profile_code) noexcept {
	return codec_profile_code == PIXEL_CODEC_PROFILE_HTJ2K_LOSSLESS_V2 ||
	    codec_profile_code == PIXEL_CODEC_PROFILE_HTJ2K_LOSSLESS_RPCL_V2 ||
	    codec_profile_code == PIXEL_CODEC_PROFILE_HTJ2K_LOSSY_V2;
}

void validate_encode_profile_source_constraints(uint32_t codec_profile_code,
    int bits_allocated, int bits_stored, std::string_view file_path) {
	if (is_16bit_limited_encode_profile(codec_profile_code) &&
	    bits_allocated > 16) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=selected encoder currently supports bits_allocated <= 16",
		    file_path);
	}
	if (codec_profile_code == PIXEL_CODEC_PROFILE_JPEG_LOSSY_V2 &&
	    bits_stored > 12) {
		diag::error_and_throw(
		    "DicomFile::set_pixel_data file={} reason=lossy JPEG transfer syntax requires bits_stored <= 12",
		    file_path);
	}
}

pixel::Photometric compute_output_photometric_for_encode_profile(
    uint32_t codec_profile_code,
    bool use_multicomponent_transform,
    pixel::Photometric source_photometric) noexcept {
	if (!use_multicomponent_transform) {
		return source_photometric;
	}
	if (codec_profile_code == PIXEL_CODEC_PROFILE_JPEG2000_LOSSLESS_V2 ||
	    codec_profile_code == PIXEL_CODEC_PROFILE_HTJ2K_LOSSLESS_V2 ||
	    codec_profile_code == PIXEL_CODEC_PROFILE_HTJ2K_LOSSLESS_RPCL_V2) {
		return pixel::Photometric::ybr_rct;
	}
	return pixel::Photometric::ybr_ict;
}

bool encode_profile_uses_lossy_compression(uint32_t codec_profile_code) noexcept {
	return codec_profile_code == PIXEL_CODEC_PROFILE_JPEG2000_LOSSY_V2 ||
	    codec_profile_code == PIXEL_CODEC_PROFILE_HTJ2K_LOSSY_V2 ||
	    codec_profile_code == PIXEL_CODEC_PROFILE_JPEGLS_NEAR_LOSSLESS_V2 ||
	    codec_profile_code == PIXEL_CODEC_PROFILE_JPEG_LOSSY_V2 ||
	    codec_profile_code == PIXEL_CODEC_PROFILE_JPEGXL_LOSSY_V2;
}

std::optional<std::string_view> lossy_method_for_encode_profile(
    uint32_t codec_profile_code) noexcept {
	if (codec_profile_code == PIXEL_CODEC_PROFILE_HTJ2K_LOSSY_V2) {
		return std::string_view("ISO_15444_15");
	}
	if (codec_profile_code == PIXEL_CODEC_PROFILE_JPEG2000_LOSSY_V2) {
		return std::string_view("ISO_15444_1");
	}
	if (codec_profile_code == PIXEL_CODEC_PROFILE_JPEGLS_NEAR_LOSSLESS_V2) {
		return std::string_view("ISO_14495_1");
	}
	if (codec_profile_code == PIXEL_CODEC_PROFILE_JPEG_LOSSY_V2) {
		return std::string_view("ISO_10918_1");
	}
	if (codec_profile_code == PIXEL_CODEC_PROFILE_JPEGXL_LOSSY_V2) {
		return std::string_view("ISO_18181_1");
	}
	return std::nullopt;
}

} // namespace dicom::pixel::detail
