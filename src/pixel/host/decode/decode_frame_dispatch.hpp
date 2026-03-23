#pragma once

#include "dicom.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace dicom::pixel::detail {

void dispatch_decode_frame(const DicomFile& df, std::size_t frame_index,
    std::span<std::uint8_t> dst, const DecodePlan& plan);
void dispatch_decode_prepared_frame(const DicomFile& df,
    const PixelLayout& source_layout,
    std::size_t frame_index, std::span<const std::uint8_t> prepared_source,
    std::span<std::uint8_t> dst, const DecodePlan& plan);

void dispatch_decode_all_frames(
    const DicomFile& df, std::span<std::uint8_t> dst, const DecodePlan& plan);
void dispatch_decode_all_frames(const DicomFile& df, std::span<std::uint8_t> dst,
    const DecodePlan& plan, const ExecutionObserver* observer);

} // namespace dicom::pixel::detail
