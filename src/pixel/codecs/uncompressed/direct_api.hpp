#pragma once

#include <cstddef>
#include <cstdint>

#include "pixel_decoder_plugin_abi.h"
#include "pixel_encoder_plugin_abi.h"

namespace pixel::core {

constexpr std::size_t kLastErrorDetailCapacity = 512;

struct ErrorState {
  char last_error_detail[kLastErrorDetailCapacity]{};
};

bool is_uncompressed_profile(uint32_t codec_profile_code);
bool is_native_uncompressed_profile(uint32_t codec_profile_code);
bool is_encapsulated_uncompressed_profile(uint32_t codec_profile_code);
pixel_supported_profile_flags supported_profile_flags();

void clear_error(ErrorState* state);
uint32_t copy_last_error_detail(
    const ErrorState* state, char* out_detail, uint32_t out_detail_capacity);

pixel_error_code decode_uncompressed_frame(
    ErrorState* state, const pixel_decoder_request* request);

pixel_error_code encode_uncompressed_frame(
    ErrorState* state, pixel_encoder_request* request);

}  // namespace pixel::core
