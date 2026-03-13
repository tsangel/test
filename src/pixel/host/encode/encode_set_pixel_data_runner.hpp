#pragma once

#include "dicom.h"

#include <functional>
#include <span>

namespace dicom::pixel::detail {

void run_set_pixel_data_with_computed_codec_options(DicomFile& file,
    uid::WellKnown transfer_syntax, const pixel::PixelSource& source,
    std::span<const CodecOptionKv> codec_options);
void run_set_pixel_data_from_frame_provider_with_computed_codec_options(
    DicomFile& file, uid::WellKnown transfer_syntax,
    const pixel::PixelSource& source_descriptor,
    std::span<const CodecOptionKv> codec_options,
    const std::function<std::span<const std::uint8_t>(std::size_t)>& frame_provider);

} // namespace dicom::pixel::detail
