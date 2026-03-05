#pragma once

#include "dicom.h"

#include <cstddef>
#include <string_view>

namespace dicom::pixel::detail {

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

} // namespace dicom::pixel::detail
