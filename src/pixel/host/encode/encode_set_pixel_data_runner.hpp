#pragma once

#include "dicom.h"

#include <functional>
#include <span>
#include <vector>

namespace dicom::pixel::detail {

void run_set_pixel_data_with_computed_codec_options(DicomFile& file,
    uid::WellKnown transfer_syntax, pixel::ConstPixelSpan source,
    std::span<const CodecOptionKv> codec_options);
void run_set_pixel_data_from_frame_provider_with_computed_codec_options(
    DicomFile& file, uid::WellKnown transfer_syntax,
    const pixel::PixelLayout& source_layout,
    std::span<const CodecOptionKv> codec_options,
    const std::function<std::span<const std::uint8_t>(std::size_t)>& frame_provider);
void run_set_pixel_data_from_frame_provider_streaming_with_computed_codec_options(
    DicomFile& file, uid::WellKnown transfer_syntax,
    const pixel::PixelLayout& source_layout,
    std::span<const CodecOptionKv> codec_options,
    const std::function<std::span<const std::uint8_t>(std::size_t)>& frame_provider);
void encode_frames_from_frame_provider_with_runtime_or_throw(
    DicomFile& file, uid::WellKnown transfer_syntax,
    const pixel::PixelLayout& source_layout, uint32_t codec_profile_code,
    std::span<const CodecOptionKv> codec_options,
    bool use_multicomponent_transform, std::size_t frame_count,
    const std::function<std::span<const std::uint8_t>(std::size_t)>& frame_provider,
    const std::function<void(std::size_t, std::vector<std::uint8_t>&&)>& frame_sink);

} // namespace dicom::pixel::detail
