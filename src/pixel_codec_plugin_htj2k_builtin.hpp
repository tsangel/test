#pragma once

#include "pixel_codec_registry.hpp"

#include <cstdint>
#include <vector>

namespace dicom::pixel::detail {

bool decode_frame_plugin_htj2k_via_abi(
    const CodecDecodeFrameInput& input, CodecError& out_error) noexcept;

bool encode_frame_plugin_htj2k_via_abi(const CodecEncodeFrameInput& input,
    std::span<const CodecOptionKv> encode_options,
    std::vector<std::uint8_t>& out_encoded_frame,
    CodecError& out_error) noexcept;

}  // namespace dicom::pixel::detail
