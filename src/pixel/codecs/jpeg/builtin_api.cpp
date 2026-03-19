#include "builtin_api.hpp"

#include "internal.hpp"

namespace pixel::jpeg_codec {

const pixel_decoder_plugin_api& decoder_builtin_api() noexcept {
  static const pixel_decoder_plugin_api api = []() {
    pixel_decoder_plugin_api out{};
    out.struct_size = sizeof(pixel_decoder_plugin_api);
    out.abi_version = PIXEL_DECODER_PLUGIN_ABI;
    out.info.struct_size = sizeof(pixel_decoder_plugin_info);
    out.info.abi_version = PIXEL_DECODER_PLUGIN_ABI;
    out.info.display_name = "JPEG Decoder (builtin)";
    out.info.supported_profile_flags = pixel::jpeg_codec::supported_profile_flags();
    out.create = &pixel::jpeg_codec::decoder_create;
    out.destroy = &pixel::jpeg_codec::decoder_destroy;
    out.configure = &pixel::jpeg_codec::decoder_configure;
    out.decode_frame = &pixel::jpeg_codec::decoder_decode_frame;
    out.copy_last_error_detail = &pixel::jpeg_codec::decoder_copy_last_error_detail;
    return out;
  }();
  return api;
}

const pixel_encoder_plugin_api& encoder_builtin_api() noexcept {
  static const pixel_encoder_plugin_api api = []() {
    pixel_encoder_plugin_api out{};
    out.struct_size = sizeof(pixel_encoder_plugin_api);
    out.abi_version = PIXEL_ENCODER_PLUGIN_ABI;
    out.info.struct_size = sizeof(pixel_encoder_plugin_info);
    out.info.abi_version = PIXEL_ENCODER_PLUGIN_ABI;
    out.info.display_name = "JPEG Encoder (builtin)";
    out.info.supported_profile_flags = pixel::jpeg_codec::supported_profile_flags();
    out.create = &pixel::jpeg_codec::encoder_create;
    out.destroy = &pixel::jpeg_codec::encoder_destroy;
    out.configure = &pixel::jpeg_codec::encoder_configure;
    out.encode_frame = &pixel::jpeg_codec::encoder_encode_frame;
    out.copy_last_error_detail = &pixel::jpeg_codec::encoder_copy_last_error_detail;
    out.encode_frame_to_context_buffer =
        &pixel::jpeg_codec::encoder_encode_frame_to_context_buffer;
    out.get_encoded_buffer = &pixel::jpeg_codec::encoder_get_encoded_buffer;
    return out;
  }();
  return api;
}

}  // namespace pixel::jpeg_codec
