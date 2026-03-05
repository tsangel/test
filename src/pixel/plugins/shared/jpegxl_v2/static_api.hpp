#pragma once

#include "pixel_decoder_plugin_abi_v2.h"
#include "pixel_encoder_plugin_abi_v2.h"

namespace pixel::jpegxl_plugin_v2 {

const pixel_decoder_plugin_api_v2& decoder_static_api() noexcept;
const pixel_encoder_plugin_api_v2& encoder_static_api() noexcept;

}  // namespace pixel::jpegxl_plugin_v2
