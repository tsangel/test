#pragma once

#include <cstddef>
#include <cstdint>

#include "pixel_decoder_plugin_abi.h"

namespace pixel::codec_common {

inline pixel_decoder_info* writable_decoder_info(
    const pixel_decoder_request* request) noexcept {
  if (request == nullptr) {
    return nullptr;
  }
  constexpr std::size_t kDecodeInfoOffset =
      offsetof(pixel_decoder_request, decode_info);
  constexpr std::size_t kRequiredRequestPrefix =
      kDecodeInfoOffset + sizeof(pixel_decoder_info*);
  if (request->struct_size < kRequiredRequestPrefix) {
    return nullptr;
  }
  pixel_decoder_info* info = request->decode_info;
  if (info == nullptr) {
    return nullptr;
  }
  if (info->struct_size < sizeof(pixel_decoder_info) ||
      info->abi_version != PIXEL_DECODER_PLUGIN_ABI) {
    return nullptr;
  }
  return info;
}

inline uint8_t decoded_planar_code_from_request(uint8_t dst_planar) noexcept {
  return dst_planar == PIXEL_PLANAR_PLANAR
      ? PIXEL_DECODED_PLANAR_PLANAR
      : PIXEL_DECODED_PLANAR_INTERLEAVED;
}

inline uint8_t default_color_space_for_sample_count(
    int32_t samples_per_pixel) noexcept {
  return samples_per_pixel == 1
      ? PIXEL_DECODED_COLOR_SPACE_MONOCHROME
      : PIXEL_DECODED_COLOR_SPACE_UNKNOWN;
}

inline void set_decoder_info(
    const pixel_decoder_request* request, uint8_t actual_color_space,
    uint8_t encoded_lossy_state, uint8_t actual_dtype, uint8_t actual_planar,
    uint16_t bits_per_sample) noexcept {
  pixel_decoder_info* info = writable_decoder_info(request);
  if (info == nullptr) {
    return;
  }
  info->actual_color_space = actual_color_space;
  info->encoded_lossy_state = encoded_lossy_state;
  info->actual_dtype = actual_dtype;
  info->actual_planar = actual_planar;
  info->bits_per_sample = bits_per_sample;
}

}  // namespace pixel::codec_common
