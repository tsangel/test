#pragma once

#include "pixel_decoder_plugin_abi_v2.h"
#include "pixel_encoder_plugin_abi_v2.h"

namespace pixel::htj2k_codec_v2 {

const pixel_decoder_plugin_api_v2& decoder_builtin_api() noexcept;
const pixel_encoder_plugin_api_v2& encoder_builtin_api() noexcept;

}  // namespace pixel::htj2k_codec_v2
