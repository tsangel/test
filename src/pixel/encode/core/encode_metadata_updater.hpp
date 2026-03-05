#pragma once

#include "dicom.h"
#include "pixel/encode/core/encode_target_resolver.hpp"

#include <cstddef>
#include <string_view>

namespace dicom::pixel::detail {

void update_pixel_metadata_for_set_pixel_data(DataSet& dataset, std::string_view file_path,
    uid::WellKnown transfer_syntax, const pixel::PixelSource& source,
    bool target_is_rle, pixel::Photometric output_photometric, int bits_allocated,
    int bits_stored, int high_bit, int pixel_representation,
    std::size_t source_row_stride, std::size_t source_frame_stride);

[[nodiscard]] std::size_t encoded_payload_size_from_pixel_sequence(
    const DataSet& dataset, std::string_view file_path, uid::WellKnown transfer_syntax);

void update_lossy_compression_metadata_for_set_pixel_data(DataSet& dataset,
    std::string_view file_path, uid::WellKnown transfer_syntax,
    const PixelEncodeTarget& target, std::size_t uncompressed_payload_bytes,
    std::size_t encoded_payload_bytes);

} // namespace dicom::pixel::detail
