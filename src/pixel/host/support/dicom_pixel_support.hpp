#pragma once

#include "dicom.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace dicom::pixel::support_detail {

struct PreparedDecodeFrameSource {
	std::span<const std::uint8_t> bytes{};
	std::vector<std::uint8_t> owned_bytes{};
};

struct NativeDecodeSourceView {
	std::span<const std::uint8_t> source_bytes{};
	std::string_view source_name{"PixelData"};
	std::size_t rows{0};
	std::size_t cols{0};
	std::size_t frames{0};
	std::size_t samples_per_pixel{0};
	bool planar_source{false};
	std::size_t bytes_per_sample{0};
	std::size_t row_payload_bytes{0};
	std::size_t frame_bytes{0};
};

[[nodiscard]] PreparedDecodeFrameSource prepare_decode_frame_source_or_throw(
    const DicomFile& df, const PixelDataInfo& info, std::size_t frame_index);

[[nodiscard]] NativeDecodeSourceView compute_native_decode_source_view_or_throw(
    const DicomFile& df, const PixelDataInfo& info);

[[nodiscard]] std::span<const std::uint8_t> native_decode_frame_bytes_or_throw(
    const NativeDecodeSourceView& source_view, std::size_t frame_index);

struct ComputedEncodeSourceLayout {
	std::size_t bytes_per_sample{0};
	std::size_t rows{0};
	std::size_t cols{0};
	std::size_t frames{0};
	std::size_t samples_per_pixel{0};
	bool planar_source{false};
	std::size_t row_payload_bytes{0};
	std::size_t source_row_stride{0};
	std::size_t source_plane_stride{0};
	std::size_t source_frame_size_bytes{0};
	std::size_t source_frame_stride{0};
	std::size_t destination_frame_payload{0};
	std::size_t destination_total_bytes{0};
	int bits_allocated{0};
	int bits_stored{0};
	int pixel_representation{0};
	int high_bit{0};
};

[[nodiscard]] ComputedEncodeSourceLayout compute_encode_source_layout_or_throw(
    const pixel::PixelSource& source, std::string_view file_path);

struct NativePixelCopyInput {
	std::span<const std::uint8_t> source_bytes{};
	std::size_t rows{0};
	std::size_t frames{0};
	std::size_t samples_per_pixel{0};
	bool planar_source{false};
	std::size_t row_payload_bytes{0};
	std::size_t source_row_stride{0};
	std::size_t source_plane_stride{0};
	std::size_t source_frame_stride{0};
	std::size_t destination_frame_payload{0};
	std::size_t destination_total_bytes{0};
};

[[nodiscard]] std::vector<std::uint8_t> build_native_pixel_payload(
    const NativePixelCopyInput& input);

[[nodiscard]] bool source_aliases_native_pixel_data(
    const DataSet& dataset, std::span<const std::uint8_t> source_bytes) noexcept;

} // namespace dicom::pixel::support_detail
