#pragma once

#include "dicom.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace dicom::pixel::detail {

struct EncapsulatedEncodeInput {
	const std::uint8_t* source_base{nullptr};
	std::size_t frame_count{0};
	std::size_t source_frame_stride{0};
	std::size_t source_frame_payload{0};
	bool source_aliases_current_native_pixel_data{false};
};

template <typename EncodeFrameFn>
inline void encode_frames_to_encapsulated_pixel_data(DicomFile& file,
    const EncapsulatedEncodeInput& input, EncodeFrameFn&& encode_frame) {
	// When source bytes alias current native PixelData, replacing PixelData first would
	// invalidate source spans. Encode all frames first, then replace PixelData.
	if (input.source_aliases_current_native_pixel_data) {
		std::vector<std::vector<std::uint8_t>> encoded_frames;
		encoded_frames.reserve(input.frame_count);
		for (std::size_t frame_index = 0; frame_index < input.frame_count; ++frame_index) {
			const auto* source_frame = input.source_base + frame_index * input.source_frame_stride;
			const auto source_frame_view =
			    std::span<const std::uint8_t>(source_frame, input.source_frame_payload);
			encoded_frames.push_back(encode_frame(source_frame_view));
		}
		file.reset_encapsulated_pixel_data(input.frame_count);
		for (std::size_t frame_index = 0; frame_index < input.frame_count; ++frame_index) {
			file.set_encoded_pixel_frame(frame_index, std::move(encoded_frames[frame_index]));
		}
		return;
	}

	file.reset_encapsulated_pixel_data(input.frame_count);
	for (std::size_t frame_index = 0; frame_index < input.frame_count; ++frame_index) {
		const auto* source_frame = input.source_base + frame_index * input.source_frame_stride;
		const auto source_frame_view =
		    std::span<const std::uint8_t>(source_frame, input.source_frame_payload);
		auto encoded_frame = encode_frame(source_frame_view);
		file.set_encoded_pixel_frame(frame_index, std::move(encoded_frame));
	}
}

// Encodes one native frame into DICOM RLE Lossless frame payload.
// The output contains the 64-byte RLE header and all segments.
std::vector<std::uint8_t> encode_rle_frame(std::span<const std::uint8_t> frame_data,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t bytes_per_sample, Planar source_planar, std::size_t row_stride,
    std::string_view file_path);

void encode_rle_pixel_data(DicomFile& file, const EncapsulatedEncodeInput& input,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t bytes_per_sample, Planar source_planar, std::size_t row_stride,
    std::string_view file_path);

// Encodes one native frame into JPEG 2000 codestream (J2K) for encapsulated PixelData.
std::vector<std::uint8_t> encode_jpeg2k_frame(std::span<const std::uint8_t> frame_data,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t bytes_per_sample, int bits_allocated, int bits_stored,
    int pixel_representation, Planar source_planar, std::size_t row_stride,
    bool use_multicomponent_transform, bool lossless,
    const J2kOptions& options, std::string_view file_path);

void encode_jpeg2k_pixel_data(DicomFile& file, const EncapsulatedEncodeInput& input,
    std::size_t rows, std::size_t cols, std::size_t samples_per_pixel,
    std::size_t bytes_per_sample, int bits_allocated, int bits_stored,
    int pixel_representation, Planar source_planar, std::size_t row_stride,
    bool use_multicomponent_transform, bool lossless, const J2kOptions& options,
    std::string_view file_path);

} // namespace dicom::pixel::detail
