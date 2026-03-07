#pragma once

#include <cstddef>
#include <cstdint>

#include "pixel_decoder_plugin_abi_v2.h"

namespace pixel::rle_plugin_v2 {

pixel_error_code_v2 decode_single_channel_frame_direct(
    const pixel_decoder_request_v2* request, char* out_detail,
    uint32_t out_detail_capacity);

}  // namespace pixel::rle_plugin_v2
