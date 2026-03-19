#include "internal.hpp"
#include "builtin_api.hpp"

namespace pixel::rle_codec {

const pixel_decoder_plugin_api& decoder_builtin_api() noexcept {
  static const pixel_decoder_plugin_api api = [] {
    pixel_decoder_plugin_api value{};
    value.struct_size = sizeof(pixel_decoder_plugin_api);
    value.abi_version = PIXEL_DECODER_PLUGIN_ABI;
    value.info.struct_size = sizeof(pixel_decoder_plugin_info);
    value.info.abi_version = PIXEL_DECODER_PLUGIN_ABI;
    value.info.display_name = "RLE Lossless Decoder (builtin)";
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

const pixel_encoder_plugin_api& encoder_builtin_api() noexcept {
  static const pixel_encoder_plugin_api api = [] {
    pixel_encoder_plugin_api value{};
    value.struct_size = sizeof(pixel_encoder_plugin_api);
    value.abi_version = PIXEL_ENCODER_PLUGIN_ABI;
    value.info.struct_size = sizeof(pixel_encoder_plugin_info);
    value.info.abi_version = PIXEL_ENCODER_PLUGIN_ABI;
    value.info.display_name = "RLE Lossless Encoder (builtin)";
    value.info.supported_profile_flags = supported_profile_flags();
    value.create = &encoder_create;
    value.destroy = &encoder_destroy;
    value.configure = &encoder_configure;
    value.encode_frame = &encoder_encode_frame;
    value.copy_last_error_detail = &encoder_copy_last_error_detail;
    value.encode_frame_to_context_buffer = &encoder_encode_frame_to_context_buffer;
    value.get_encoded_buffer = &encoder_get_encoded_buffer;
    return value;
  }();
  return api;
}

}  // namespace pixel::rle_codec
