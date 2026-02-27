#pragma once

#include "pixel/registry/codec_registry.hpp"
#include "pixel_decoder_plugin_abi.h"
#include "pixel_encoder_plugin_abi.h"

#include <cstdint>
#include <vector>

namespace dicom::pixel::detail {

bool decode_frame_plugin_htj2k_via_abi(
    const CodecDecodeFrameInput& input, CodecError& out_error) noexcept;
bool decode_frame_plugin_htj2k_via_abi_auto(
    const CodecDecodeFrameInput& input, CodecError& out_error) noexcept;
bool decode_frame_plugin_htj2k_via_abi_openjph(
    const CodecDecodeFrameInput& input, CodecError& out_error) noexcept;
bool decode_frame_plugin_htj2k_via_abi_openjpeg(
    const CodecDecodeFrameInput& input, CodecError& out_error) noexcept;

[[nodiscard]] codec_decode_frame_fn htj2k_decode_dispatch_for_backend(
    Htj2kDecoder backend) noexcept;
[[nodiscard]] Htj2kDecoder htj2k_decoder_backend_for_dispatch(
    codec_decode_frame_fn decode_dispatch) noexcept;
[[nodiscard]] bool is_builtin_htj2k_decode_dispatch(
    codec_decode_frame_fn decode_dispatch) noexcept;

bool encode_frame_plugin_htj2k_via_abi(const CodecEncodeFrameInput& input,
    std::span<const CodecOptionKv> encode_options,
    std::vector<std::uint8_t>& out_encoded_frame,
    CodecError& out_error) noexcept;

[[nodiscard]] const dicomsdl_decoder_plugin_api_v1&
htj2k_decoder_plugin_api_for_shared() noexcept;
[[nodiscard]] const dicomsdl_encoder_plugin_api_v1&
htj2k_encoder_plugin_api_for_shared() noexcept;

}  // namespace dicom::pixel::detail
