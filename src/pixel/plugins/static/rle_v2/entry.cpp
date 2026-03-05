#include "internal.hpp"
#include "static_api.hpp"

namespace pixel::rle_plugin_v2 {

const pixel_decoder_plugin_api_v2& decoder_static_api() noexcept {
  static const pixel_decoder_plugin_api_v2 api = [] {
    pixel_decoder_plugin_api_v2 value{};
    value.struct_size = sizeof(pixel_decoder_plugin_api_v2);
    value.abi_version = PIXEL_DECODER_PLUGIN_ABI_V2;
    value.info.struct_size = sizeof(pixel_decoder_plugin_info_v2);
    value.info.abi_version = PIXEL_DECODER_PLUGIN_ABI_V2;
    value.info.display_name = "RLE Lossless v2 Decoder (Static)";
    value.info.supported_profile_flags = supported_profile_flags();
    value.create = &decoder_create;
    value.destroy = &decoder_destroy;
    value.configure = &decoder_configure;
    value.decode_frame = &decoder_decode_frame;
    value.copy_last_error_detail = &decoder_copy_last_error_detail;
    return value;
  }();
  return api;
}

const pixel_encoder_plugin_api_v2& encoder_static_api() noexcept {
  static const pixel_encoder_plugin_api_v2 api = [] {
    pixel_encoder_plugin_api_v2 value{};
    value.struct_size = sizeof(pixel_encoder_plugin_api_v2);
    value.abi_version = PIXEL_ENCODER_PLUGIN_ABI_V2;
    value.info.struct_size = sizeof(pixel_encoder_plugin_info_v2);
    value.info.abi_version = PIXEL_ENCODER_PLUGIN_ABI_V2;
    value.info.display_name = "RLE Lossless v2 Encoder (Static)";
    value.info.supported_profile_flags = supported_profile_flags();
    value.create = &encoder_create;
    value.destroy = &encoder_destroy;
    value.configure = &encoder_configure;
    value.encode_frame = &encoder_encode_frame;
    value.copy_last_error_detail = &encoder_copy_last_error_detail;
    return value;
  }();
  return api;
}

}  // namespace pixel::rle_plugin_v2
