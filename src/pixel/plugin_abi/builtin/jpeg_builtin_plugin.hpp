#pragma once

#include "pixel/plugin_abi/builtin/shared_plugin_export.hpp"
#include "pixel/registry/codec_registry.hpp"
#include "pixel_decoder_plugin_abi.h"
#include "pixel_encoder_plugin_abi.h"

#include <cstdint>
#include <vector>

namespace dicom::pixel::detail {

bool decode_frame_plugin_jpeg_via_abi(
    const CodecDecodeFrameInput& input, CodecError& out_error) noexcept;

bool encode_frame_plugin_jpeg_via_abi(const CodecEncodeFrameInput& input,
    std::span<const CodecOptionKv> encode_options,
    std::vector<std::uint8_t>& out_encoded_frame,
    CodecError& out_error) noexcept;

[[nodiscard]] const dicomsdl_decoder_plugin_api_v1&
DICOMSDL_CODEC_RUNTIME_API jpeg_decoder_plugin_api_for_shared() noexcept;
[[nodiscard]] const dicomsdl_encoder_plugin_api_v1&
DICOMSDL_CODEC_RUNTIME_API jpeg_encoder_plugin_api_for_shared() noexcept;

}  // namespace dicom::pixel::detail
