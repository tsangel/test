#pragma once

#include "pixel_decoder_plugin_abi_v2.h"
#include "pixel_encoder_plugin_abi_v2.h"

namespace pixel {
namespace rle_plugin_v2 {

const pixel_decoder_plugin_api_v2& decoder_static_api() noexcept;
const pixel_encoder_plugin_api_v2& encoder_static_api() noexcept;

}  // namespace rle_plugin_v2
}  // namespace pixel
