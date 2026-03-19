#pragma once

#include "pixel_decoder_plugin_abi.h"
#include "pixel_encoder_plugin_abi.h"

namespace pixel {
namespace rle_codec {

const pixel_decoder_plugin_api& decoder_builtin_api() noexcept;
const pixel_encoder_plugin_api& encoder_builtin_api() noexcept;

}  // namespace rle_codec
}  // namespace pixel
