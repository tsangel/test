#pragma once

#include "pixel/decode/core/decode_codec_impl_detail.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace dicom::pixel::detail {

void dispatch_decode_frame_with_resolved_transform(const DicomFile& df,
    const DecodeValueTransform& value_transform, std::size_t frame_index,
    std::span<std::uint8_t> dst, const DecodeStrides& dst_strides,
    const DecodeOptions& effective_opt);

} // namespace dicom::pixel::detail
