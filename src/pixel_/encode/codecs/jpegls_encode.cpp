#include "pixel_/encode/core/encode_codec_impl_detail.hpp"

#include <charls/charls.h>

#include <cstdint>
#include <exception>
#include <limits>
#include <string>
#include <utility>

namespace dicom::pixel::detail {
namespace {

} // namespace

bool try_encode_jpegls_frame(std::span<const std::uint8_t> frame_data,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t bytes_per_sample, int bits_allocated, int bits_stored, Planar source_planar,
    std::size_t row_stride, int near_lossless_error, std::vector<std::uint8_t>& out_encoded,
    CodecError& out_error) noexcept {
	out_encoded.clear();
	out_error = CodecError{};
	if (samples_per_pixel != 1 && samples_per_pixel != 3 && samples_per_pixel != 4) {
		set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
		    "only samples_per_pixel=1/3/4 are supported in current JPEG-LS encoder path");
		return false;
	}
	if (bits_allocated <= 0 || bits_allocated > 16 || (bits_allocated % 8) != 0) {
		set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
		    "bits_allocated must be 8 or 16");
		return false;
	}
	if (bits_stored <= 0 || bits_stored > bits_allocated) {
		set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
		    "bits_stored must be in [1, bits_allocated]");
		return false;
	}
	if (near_lossless_error < 0 || near_lossless_error > 255) {
		set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
		    "near_lossless_error must be in [0,255]");
		return false;
	}

	try {
		const auto source_layout = make_source_frame_layout(frame_data, rows, cols, samples_per_pixel,
		    bytes_per_sample, source_planar, row_stride);
		if (row_stride > std::numeric_limits<std::uint32_t>::max()) {
			set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
			    "row_stride exceeds uint32_t range for CharLS");
			return false;
		}
		if (cols > std::numeric_limits<std::uint32_t>::max() ||
		    rows > std::numeric_limits<std::uint32_t>::max() ||
		    samples_per_pixel > std::numeric_limits<int32_t>::max()) {
			set_codec_error(out_error, CodecStatusCode::invalid_argument, "validate",
			    "rows/cols/samples_per_pixel exceed CharLS range");
			return false;
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

		out_encoded.assign(encoder.estimated_destination_size(), std::uint8_t{0});
		encoder.destination(out_encoded);

		const auto bytes_written =
		    encoder.encode(frame_data.data(), source_layout.minimum_frame_bytes,
		        static_cast<std::uint32_t>(row_stride));
		out_encoded.resize(bytes_written);
	} catch (const std::exception& e) {
		set_codec_error(out_error, CodecStatusCode::backend_error, "encode",
		    std::string("CharLS encode failed (") + e.what() + ")");
		return false;
	} catch (...) {
		set_codec_error(out_error, CodecStatusCode::backend_error, "encode",
		    "CharLS encode failed (non-standard exception)");
		return false;
	}

	if (out_encoded.empty()) {
		set_codec_error(out_error, CodecStatusCode::backend_error, "encode",
		    "CharLS produced empty codestream");
		return false;
	}
	out_error = CodecError{};
	return true;
}

} // namespace dicom::pixel::detail
