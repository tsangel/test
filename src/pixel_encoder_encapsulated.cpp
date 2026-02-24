#include "pixel_encoder_detail.hpp"

#include "diagnostics.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

namespace dicom::pixel::detail {
using namespace dicom::literals;
namespace diag = dicom::diag;

namespace {

[[nodiscard]] std::vector<std::uint8_t> copy_single_frame_to_native_layout(
    std::span<const std::uint8_t> source_bytes, std::size_t rows, std::size_t samples_per_pixel,
    bool planar_source, std::size_t row_payload_bytes, std::size_t source_row_stride,
    std::size_t source_plane_stride, std::size_t destination_frame_payload) {
	std::vector<std::uint8_t> native_pixel_data(destination_frame_payload);
	if (native_pixel_data.empty()) {
		return native_pixel_data;
	}

	const auto* source_base = source_bytes.data();
	auto* destination_base = native_pixel_data.data();

	if (planar_source) {
		const std::size_t destination_plane_stride =
		    destination_frame_payload / samples_per_pixel;
		for (std::size_t sample = 0; sample < samples_per_pixel; ++sample) {
			const auto* source_plane = source_base + sample * source_plane_stride;
			auto* destination_plane = destination_base + sample * destination_plane_stride;
			for (std::size_t row = 0; row < rows; ++row) {
				std::memcpy(destination_plane + row * row_payload_bytes,
				    source_plane + row * source_row_stride, row_payload_bytes);
			}
		}
		return native_pixel_data;
	}

	for (std::size_t row = 0; row < rows; ++row) {
		std::memcpy(destination_base + row * row_payload_bytes,
		    source_base + row * source_row_stride, row_payload_bytes);
	}
	return native_pixel_data;
}

} // namespace

void encode_encapsulated_pixel_data(const EncapsulatedPixelEncodeDispatchInput& input) {
	if (input.is_encapsulated_uncompressed) {
		encode_frames_to_encapsulated_pixel_data(
		    input.file, input.encode_input, [&](std::span<const std::uint8_t> source_frame_view) {
			    return copy_single_frame_to_native_layout(source_frame_view, input.rows,
			        input.samples_per_pixel, input.planar_source, input.row_payload_bytes,
			        input.source_row_stride, input.source_plane_stride,
			        input.destination_frame_payload);
		    });
		return;
	}

	if (input.is_j2k) {
		const auto& j2k_options = std::get<J2kOptions>(input.resolved_codec_opt);
		encode_jpeg2k_pixel_data(input.file, input.encode_input, input.rows, input.cols,
		    input.samples_per_pixel, input.bytes_per_sample, input.bits_allocated,
		    input.bits_stored, input.pixel_representation, input.source_planar,
		    input.source_row_stride, input.use_multicomponent_transform,
		    input.is_j2k_lossless, j2k_options, input.file_path);
		return;
	}

	if (input.is_htj2k) {
		const bool rpcl_progression = input.transfer_syntax == "HTJ2KLosslessRPCL"_uid;
		const auto& htj2k_options = std::get<Htj2kOptions>(input.resolved_codec_opt);
		encode_htj2k_pixel_data(input.file, input.encode_input, input.rows, input.cols,
		    input.samples_per_pixel, input.bytes_per_sample, input.bits_allocated,
		    input.bits_stored, input.pixel_representation, input.source_planar,
		    input.source_row_stride, input.use_multicomponent_transform,
		    input.is_htj2k_lossless, rpcl_progression, htj2k_options, input.file_path);
		return;
	}

	if (input.is_jpegls) {
		const auto& jpegls_options = std::get<JpegLsOptions>(input.resolved_codec_opt);
		encode_jpegls_pixel_data(input.file, input.encode_input, input.rows, input.cols,
		    input.samples_per_pixel, input.bytes_per_sample, input.bits_allocated,
		    input.bits_stored, input.source_planar, input.source_row_stride,
		    jpegls_options.near_lossless_error, input.file_path);
		return;
	}

	if (input.is_jpeg) {
		const auto& jpeg_options = std::get<JpegOptions>(input.resolved_codec_opt);
		encode_jpeg_pixel_data(input.file, input.encode_input, input.rows, input.cols,
		    input.samples_per_pixel, input.bytes_per_sample, input.bits_allocated,
		    input.bits_stored, input.source_planar, input.source_row_stride,
		    input.is_jpeg_lossless, jpeg_options, input.file_path);
		return;
	}

	if (input.is_rle) {
		encode_rle_pixel_data(input.file, input.encode_input, input.rows, input.cols,
		    input.samples_per_pixel, input.bytes_per_sample, input.source_planar,
		    input.source_row_stride, input.file_path);
		return;
	}

	diag::error_and_throw(
	    "DicomFile::set_pixel_data file={} ts={} reason=internal codec dispatch could not resolve an encoder path",
	    input.file_path, input.transfer_syntax.value());
}

} // namespace dicom::pixel::detail
