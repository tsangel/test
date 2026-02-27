#pragma once

#include "dicom.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace dicom::pixel::detail {

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

} // namespace dicom::pixel::detail
