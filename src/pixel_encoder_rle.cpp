#include "pixel_encoder_detail.hpp"

#include "dicom_endian.h"
#include "diagnostics.h"

#include <array>
#include <limits>

namespace dicom::pixel::detail {
namespace diag = dicom::diag;
namespace {

[[nodiscard]] std::vector<std::uint8_t> encode_packbits_segment(
    std::span<const std::uint8_t> source) {
	std::vector<std::uint8_t> encoded;
	encoded.reserve(source.size() + source.size() / 128 + 16);

	std::size_t i = 0;
	while (i < source.size()) {
		std::size_t repeat_count = 1;
		while (i + repeat_count < source.size() && repeat_count < 128 &&
		       source[i + repeat_count] == source[i]) {
			++repeat_count;
		}

		if (repeat_count >= 2) {
			const auto control = static_cast<std::int8_t>(1 - static_cast<int>(repeat_count));
			encoded.push_back(static_cast<std::uint8_t>(control));
			encoded.push_back(source[i]);
			i += repeat_count;
			continue;
		}

		const std::size_t literal_begin = i;
		std::size_t literal_count = 1;
		++i;

		while (i < source.size() && literal_count < 128) {
			repeat_count = 1;
			while (i + repeat_count < source.size() && repeat_count < 128 &&
			       source[i + repeat_count] == source[i]) {
				++repeat_count;
			}
			if (repeat_count >= 2) {
				break;
			}
			++i;
			++literal_count;
		}

		encoded.push_back(static_cast<std::uint8_t>(literal_count - 1));
		encoded.insert(encoded.end(), source.begin() + static_cast<std::ptrdiff_t>(literal_begin),
		    source.begin() + static_cast<std::ptrdiff_t>(literal_begin + literal_count));
	}

	return encoded;
}

} // namespace

std::vector<std::uint8_t> encode_rle_frame(std::span<const std::uint8_t> frame_data,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t bytes_per_sample, Planar source_planar, std::size_t row_stride,
    std::string_view file_path) {
	constexpr std::string_view kFunctionName = "pixel::encode_rle_frame";

	if (samples_per_pixel == 0 || bytes_per_sample == 0 ||
	    bytes_per_sample > 15 || samples_per_pixel > (15 / bytes_per_sample)) {
		diag::error_and_throw(
		    "pixel::encode_rle_frame file={} reason=unsupported RLE segment layout (spp={}, bytes_per_sample={})",
		    file_path, samples_per_pixel, bytes_per_sample);
	}
	const std::size_t segment_count = samples_per_pixel * bytes_per_sample;

	const auto source_layout = make_source_frame_layout(frame_data, rows, cols, samples_per_pixel,
	    bytes_per_sample, source_planar, row_stride, kFunctionName, file_path);
	const bool source_is_planar = source_layout.source_is_planar;
	const std::size_t plane_stride = source_layout.plane_stride;

	const std::size_t pixels_per_plane = rows * cols;
	std::vector<std::uint8_t> byte_plane(pixels_per_plane, 0);
	std::vector<std::vector<std::uint8_t>> segments(segment_count);
	const std::size_t interleaved_pixel_stride = source_layout.interleaved_pixel_stride;

	const auto* frame_ptr = frame_data.data();
	for (std::size_t sample = 0; sample < samples_per_pixel; ++sample) {
		for (std::size_t byte_plane_index = 0; byte_plane_index < bytes_per_sample; ++byte_plane_index) {
			const auto component_byte_index = bytes_per_sample - 1 - byte_plane_index;
			for (std::size_t row = 0; row < rows; ++row) {
				const std::uint8_t* src_row = nullptr;
				if (source_is_planar) {
					src_row = frame_ptr + sample * plane_stride + row * row_stride;
					for (std::size_t col = 0; col < cols; ++col) {
						byte_plane[row * cols + col] =
						    src_row[col * bytes_per_sample + component_byte_index];
					}
				} else {
					src_row = frame_ptr + row * row_stride;
					const std::size_t sample_offset = sample * bytes_per_sample;
					for (std::size_t col = 0; col < cols; ++col) {
						byte_plane[row * cols + col] =
						    src_row[col * interleaved_pixel_stride + sample_offset + component_byte_index];
					}
				}
			}

			const std::size_t segment_index = sample * bytes_per_sample + byte_plane_index;
			segments[segment_index] = encode_packbits_segment(byte_plane);
		}
	}

	std::size_t encoded_payload_size = 64;
	for (const auto& segment : segments) {
		if (segment.size() > std::numeric_limits<std::uint32_t>::max() - encoded_payload_size) {
			diag::error_and_throw(
			    "pixel::encode_rle_frame file={} reason=RLE frame size exceeds 32-bit range",
			    file_path);
		}
		encoded_payload_size += segment.size();
	}

	std::vector<std::uint8_t> encoded_frame(64, 0);
	encoded_frame.reserve(encoded_payload_size);
	endian::store_le<std::uint32_t>(encoded_frame.data(), static_cast<std::uint32_t>(segment_count));

	std::uint32_t segment_offset = 64;
	for (std::size_t i = 0; i < segment_count; ++i) {
		endian::store_le<std::uint32_t>(
		    encoded_frame.data() + 4 + i * sizeof(std::uint32_t), segment_offset);
		const auto segment_size = segments[i].size();
		if (segment_size > std::numeric_limits<std::uint32_t>::max() - segment_offset) {
			diag::error_and_throw(
			    "pixel::encode_rle_frame file={} reason=RLE segment offsets exceed 32-bit range",
			    file_path);
		}
		segment_offset += static_cast<std::uint32_t>(segment_size);
	}

	for (const auto& segment : segments) {
		encoded_frame.insert(encoded_frame.end(), segment.begin(), segment.end());
	}
	return encoded_frame;
}

void encode_rle_pixel_data(DicomFile& file, const EncapsulatedEncodeInput& input,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t bytes_per_sample, Planar source_planar, std::size_t row_stride,
    std::string_view file_path) {
	encode_frames_to_encapsulated_pixel_data(
	    file, input, [&](std::span<const std::uint8_t> source_frame_view) {
		    return encode_rle_frame(source_frame_view, rows, cols, samples_per_pixel,
		        bytes_per_sample, source_planar, row_stride, file_path);
	    });
}

} // namespace dicom::pixel::detail
