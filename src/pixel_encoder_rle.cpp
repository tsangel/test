#include "pixel_encoder_detail.hpp"

#include "dicom_endian.h"

#include <array>
#include <exception>
#include <limits>
#include <new>
#include <string>

namespace dicom::pixel::detail {
namespace {

void set_codec_error(codec_error& out_error, codec_status_code code,
    std::string_view stage, std::string detail) {
	out_error.code = code;
	out_error.stage = std::string(stage);
	out_error.detail = std::move(detail);
}

[[nodiscard]] bool checked_mul_size_t(std::size_t lhs, std::size_t rhs,
    std::size_t& out) noexcept {
	if (lhs == 0 || rhs == 0) {
		out = 0;
		return true;
	}
	if (lhs > std::numeric_limits<std::size_t>::max() / rhs) {
		return false;
	}
	out = lhs * rhs;
	return true;
}

struct RleSourceLayout {
	bool source_is_planar{false};
	std::size_t plane_stride{0};
	std::size_t interleaved_pixel_stride{0};
	std::size_t minimum_frame_bytes{0};
};

bool try_resolve_rle_source_layout(std::span<const std::uint8_t> frame_data,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t bytes_per_sample, Planar source_planar, std::size_t row_stride,
    RleSourceLayout& out_layout, codec_error& out_error) noexcept {
	if (rows == 0 || cols == 0 || samples_per_pixel == 0 || bytes_per_sample == 0) {
		set_codec_error(out_error, codec_status_code::invalid_argument, "validate",
		    "rows/cols/samples_per_pixel/bytes_per_sample must be positive");
		return false;
	}
	if (rows > 65535 || cols > 65535) {
		set_codec_error(out_error, codec_status_code::invalid_argument, "validate",
		    "rows/cols must be <= 65535 for DICOM pixel data");
		return false;
	}
	if (samples_per_pixel > 16) {
		set_codec_error(out_error, codec_status_code::invalid_argument, "validate",
		    "samples_per_pixel is outside practical encoder range");
		return false;
	}
	if (bytes_per_sample > 8) {
		set_codec_error(out_error, codec_status_code::invalid_argument, "validate",
		    "bytes_per_sample is outside practical encoder range");
		return false;
	}
	if (row_stride > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
		set_codec_error(out_error, codec_status_code::invalid_argument, "validate",
		    "row_stride exceeds 32-bit range");
		return false;
	}

	const bool source_is_planar =
	    source_planar == Planar::planar && samples_per_pixel > std::size_t{1};
	std::size_t row_components = cols;
	if (!source_is_planar &&
	    !checked_mul_size_t(cols, samples_per_pixel, row_components)) {
		set_codec_error(out_error, codec_status_code::invalid_argument, "validate",
		    "row component count exceeds size_t range");
		return false;
	}

	std::size_t row_payload = 0;
	if (!checked_mul_size_t(row_components, bytes_per_sample, row_payload)) {
		set_codec_error(out_error, codec_status_code::invalid_argument, "validate",
		    "row payload exceeds size_t range");
		return false;
	}
	if (row_stride < row_payload) {
		set_codec_error(out_error, codec_status_code::invalid_argument, "validate",
		    "row_stride is smaller than row payload");
		return false;
	}

	std::size_t plane_stride = 0;
	if (!checked_mul_size_t(row_stride, rows, plane_stride)) {
		set_codec_error(out_error, codec_status_code::invalid_argument, "validate",
		    "plane stride exceeds size_t range");
		return false;
	}

	std::size_t minimum_frame_bytes = plane_stride;
	if (source_is_planar &&
	    !checked_mul_size_t(plane_stride, samples_per_pixel, minimum_frame_bytes)) {
		set_codec_error(out_error, codec_status_code::invalid_argument, "validate",
		    "minimum frame bytes exceeds size_t range");
		return false;
	}
	if (frame_data.size() < minimum_frame_bytes) {
		set_codec_error(out_error, codec_status_code::invalid_argument, "validate",
		    "frame_data is shorter than required frame size");
		return false;
	}

	std::size_t interleaved_pixel_stride = 0;
	if (!source_is_planar &&
	    !checked_mul_size_t(samples_per_pixel, bytes_per_sample,
	        interleaved_pixel_stride)) {
		set_codec_error(out_error, codec_status_code::invalid_argument, "validate",
		    "interleaved pixel stride exceeds size_t range");
		return false;
	}

	out_layout.source_is_planar = source_is_planar;
	out_layout.plane_stride = plane_stride;
	out_layout.interleaved_pixel_stride = interleaved_pixel_stride;
	out_layout.minimum_frame_bytes = minimum_frame_bytes;
	return true;
}

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

bool try_encode_rle_frame(std::span<const std::uint8_t> frame_data,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t bytes_per_sample, Planar source_planar, std::size_t row_stride,
    std::vector<std::uint8_t>& out_encoded, codec_error& out_error) noexcept {
	out_encoded.clear();
	out_error = codec_error{};

	if (samples_per_pixel == 0 || bytes_per_sample == 0 ||
	    bytes_per_sample > 15 || samples_per_pixel > (15 / bytes_per_sample)) {
		set_codec_error(out_error, codec_status_code::invalid_argument, "validate",
		    "unsupported RLE segment layout");
		return false;
	}

	RleSourceLayout source_layout{};
	if (!try_resolve_rle_source_layout(frame_data, rows, cols, samples_per_pixel,
	        bytes_per_sample, source_planar, row_stride, source_layout, out_error)) {
		return false;
	}

	try {
		const std::size_t segment_count = samples_per_pixel * bytes_per_sample;
		std::size_t pixels_per_plane = 0;
		if (!checked_mul_size_t(rows, cols, pixels_per_plane)) {
			set_codec_error(out_error, codec_status_code::invalid_argument,
			    "validate", "rows*cols exceeds size_t range");
			return false;
		}

		std::vector<std::uint8_t> byte_plane(pixels_per_plane, 0);
		std::vector<std::vector<std::uint8_t>> segments(segment_count);

		const auto* frame_ptr = frame_data.data();
		for (std::size_t sample = 0; sample < samples_per_pixel; ++sample) {
			for (std::size_t byte_plane_index = 0; byte_plane_index < bytes_per_sample;
			     ++byte_plane_index) {
				const auto component_byte_index = bytes_per_sample - 1 - byte_plane_index;
				for (std::size_t row = 0; row < rows; ++row) {
					const std::uint8_t* src_row = nullptr;
					if (source_layout.source_is_planar) {
						src_row = frame_ptr + sample * source_layout.plane_stride +
						    row * row_stride;
						for (std::size_t col = 0; col < cols; ++col) {
							byte_plane[row * cols + col] =
							    src_row[col * bytes_per_sample + component_byte_index];
						}
					} else {
						src_row = frame_ptr + row * row_stride;
						const std::size_t sample_offset = sample * bytes_per_sample;
						for (std::size_t col = 0; col < cols; ++col) {
							byte_plane[row * cols + col] =
							    src_row[col * source_layout.interleaved_pixel_stride +
							        sample_offset + component_byte_index];
						}
					}
				}

				const std::size_t segment_index =
				    sample * bytes_per_sample + byte_plane_index;
				segments[segment_index] = encode_packbits_segment(byte_plane);
			}
		}

		constexpr std::size_t kMaxOffset =
		    static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());
		std::size_t encoded_payload_size = 64;
		for (const auto& segment : segments) {
			if (encoded_payload_size > kMaxOffset ||
			    segment.size() > kMaxOffset - encoded_payload_size) {
				set_codec_error(out_error, codec_status_code::invalid_argument,
				    "encode", "RLE frame size exceeds 32-bit range");
				return false;
			}
			encoded_payload_size += segment.size();
		}

		std::vector<std::uint8_t> encoded_frame(64, 0);
		encoded_frame.reserve(encoded_payload_size);
		endian::store_le<std::uint32_t>(
		    encoded_frame.data(), static_cast<std::uint32_t>(segment_count));

		std::uint32_t segment_offset = 64;
		for (std::size_t i = 0; i < segment_count; ++i) {
			endian::store_le<std::uint32_t>(
			    encoded_frame.data() + 4 + i * sizeof(std::uint32_t), segment_offset);
			const auto segment_size = segments[i].size();
			if (segment_size >
			    static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max() -
			                             segment_offset)) {
				set_codec_error(out_error, codec_status_code::invalid_argument,
				    "encode", "RLE segment offsets exceed 32-bit range");
				return false;
			}
			segment_offset += static_cast<std::uint32_t>(segment_size);
		}

		for (const auto& segment : segments) {
			encoded_frame.insert(encoded_frame.end(), segment.begin(), segment.end());
		}
		out_encoded = std::move(encoded_frame);
		out_error = codec_error{};
		return true;
	} catch (const std::bad_alloc&) {
		set_codec_error(out_error, codec_status_code::internal_error, "allocate",
		    "memory allocation failed");
	} catch (const std::exception& e) {
		set_codec_error(out_error, codec_status_code::backend_error, "encode",
		    e.what());
	} catch (...) {
		set_codec_error(out_error, codec_status_code::backend_error, "encode",
		    "non-standard exception");
	}
	out_encoded.clear();
	return false;
}

} // namespace dicom::pixel::detail
