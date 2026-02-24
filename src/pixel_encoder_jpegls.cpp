#include "pixel_encoder_detail.hpp"

#include "diagnostics.h"

#include <charls/charls.h>

#include <cstdint>
#include <exception>
#include <limits>

namespace dicom::pixel::detail {
namespace diag = dicom::diag;

std::vector<std::uint8_t> encode_jpegls_frame(std::span<const std::uint8_t> frame_data,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t bytes_per_sample, int bits_allocated, int bits_stored, Planar source_planar,
    std::size_t row_stride, int near_lossless_error, std::string_view file_path) {
	constexpr std::string_view kFunctionName = "pixel::encode_jpegls_frame";
	if (samples_per_pixel != 1 && samples_per_pixel != 3 && samples_per_pixel != 4) {
		diag::error_and_throw(
		    "{} file={} reason=only samples_per_pixel=1/3/4 are supported in current JPEG-LS encoder path",
		    kFunctionName, file_path);
	}
	if (bits_allocated <= 0 || bits_allocated > 16 || (bits_allocated % 8) != 0) {
		diag::error_and_throw(
		    "{} file={} reason=bits_allocated={} is not supported (supported: 8 or 16)",
		    kFunctionName, file_path, bits_allocated);
	}
	if (bits_stored <= 0 || bits_stored > bits_allocated) {
		diag::error_and_throw(
		    "{} file={} reason=bits_stored={} must be in [1, bits_allocated={}]",
		    kFunctionName, file_path, bits_stored, bits_allocated);
	}
	if (near_lossless_error < 0 || near_lossless_error > 255) {
		diag::error_and_throw(
		    "{} file={} reason=near_lossless_error={} must be in [0, 255]",
		    kFunctionName, file_path, near_lossless_error);
	}

	const auto source_layout = make_source_frame_layout(frame_data, rows, cols, samples_per_pixel,
	    bytes_per_sample, source_planar, row_stride, kFunctionName, file_path);
	if (row_stride > std::numeric_limits<std::uint32_t>::max()) {
		diag::error_and_throw(
		    "{} file={} reason=row_stride={} exceeds uint32_t range for CharLS",
		    kFunctionName, file_path, row_stride);
	}
	if (cols > std::numeric_limits<std::uint32_t>::max() ||
	    rows > std::numeric_limits<std::uint32_t>::max() ||
	    samples_per_pixel > std::numeric_limits<int32_t>::max()) {
		diag::error_and_throw(
		    "{} file={} reason=rows/cols/samples_per_pixel exceed CharLS range",
		    kFunctionName, file_path);
	}

	charls::jpegls_encoder encoder{};
	encoder.frame_info(charls::frame_info{
	    static_cast<std::uint32_t>(cols),
	    static_cast<std::uint32_t>(rows),
	    bits_stored,
	    static_cast<int32_t>(samples_per_pixel)});
	encoder.near_lossless(near_lossless_error);
	if (samples_per_pixel > std::size_t{1}) {
		const auto interleave = source_layout.source_is_planar
		                            ? charls::interleave_mode::none
		                            : charls::interleave_mode::sample;
		encoder.interleave_mode(interleave);
	}

	std::vector<std::uint8_t> encoded(encoder.estimated_destination_size());
	encoder.destination(encoded);

	try {
		const auto bytes_written =
		    encoder.encode(frame_data.data(), source_layout.minimum_frame_bytes,
		        static_cast<std::uint32_t>(row_stride));
		encoded.resize(bytes_written);
	} catch (const std::exception& e) {
		diag::error_and_throw(
		    "{} file={} reason=CharLS encode failed ({})",
		    kFunctionName, file_path, e.what());
	}
	if (encoded.empty()) {
		diag::error_and_throw(
		    "{} file={} reason=CharLS produced empty codestream",
		    kFunctionName, file_path);
	}
	return encoded;
}

void encode_jpegls_pixel_data(DicomFile& file, const EncapsulatedEncodeInput& input,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t bytes_per_sample, int bits_allocated, int bits_stored, Planar source_planar,
    std::size_t row_stride, int near_lossless_error, std::string_view file_path) {
	encode_frames_to_encapsulated_pixel_data(
	    file, input, [&](std::span<const std::uint8_t> source_frame_view) {
		    return encode_jpegls_frame(source_frame_view, rows, cols, samples_per_pixel,
		        bytes_per_sample, bits_allocated, bits_stored, source_planar,
		        row_stride, near_lossless_error, file_path);
	    });
}

} // namespace dicom::pixel::detail
