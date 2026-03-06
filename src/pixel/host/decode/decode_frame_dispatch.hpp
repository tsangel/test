#pragma once

#include "dicom.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace dicom::pixel::detail {

void dispatch_decode_frame(const DicomFile& df, std::size_t frame_index,
    std::span<std::uint8_t> dst, const DecodePlan& plan);

} // namespace dicom::pixel::detail
