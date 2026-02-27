#pragma once

#include "dicom.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace dicom::pixel::detail {

[[nodiscard]] bool should_use_scaled_output_with_resolved_options(
    const DicomFile& df, const DecodeOptions& opt);

void run_decode_frame_with_resolved_options(const DicomFile& df,
    std::size_t frame_index, std::span<std::uint8_t> dst,
    const DecodeOptions& opt);

void run_decode_frame_with_resolved_options(const DicomFile& df,
    std::size_t frame_index, std::span<std::uint8_t> dst,
    const DecodeStrides& dst_strides, const DecodeOptions& opt);

} // namespace dicom::pixel::detail
